use proc_macro::TokenStream;
use proc_macro2::{Literal, Span, TokenStream as TokenStream2};
use quote::{format_ident, quote};
use syn::{
    parse::{Parse, ParseStream},
    parse_macro_input, FnArg, ItemFn, LitStr, PatType, ReturnType, Token, Type, TypePath,
};

/* -------------------------------------------------------------------------
 * Attribute arguments: #[lua_export] or #[lua_export(name = "foo")]
 * ------------------------------------------------------------------------- */

struct MacroArgs {
    lua_name: Option<String>,
}

impl Parse for MacroArgs {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        if input.is_empty() {
            return Ok(MacroArgs { lua_name: None });
        }
        let key: syn::Ident = input.parse()?;
        if key != "name" {
            return Err(syn::Error::new(key.span(), "expected `name = \"...\"`"));
        }
        let _: Token![=] = input.parse()?;
        let lit: LitStr = input.parse()?;
        Ok(MacroArgs { lua_name: Some(lit.value()) })
    }
}

/* -------------------------------------------------------------------------
 * Type mapping
 * ------------------------------------------------------------------------- */

#[derive(Clone, Copy, PartialEq)]
enum LuaType {
    Void,
    I32,
    U32,
    F32,
    Bool,
}

impl LuaType {
    fn type_code(self) -> u8 {
        match self {
            LuaType::Void => 0,
            LuaType::I32  => 1,
            LuaType::U32  => 2,
            LuaType::F32  => 3,
            LuaType::Bool => 4,
        }
    }
}

fn parse_lua_type(ty: &Type) -> syn::Result<LuaType> {
    match ty {
        Type::Tuple(t) if t.elems.is_empty() => Ok(LuaType::Void),
        Type::Path(TypePath { path, .. }) => {
            let seg = path.segments.last()
                .ok_or_else(|| syn::Error::new_spanned(ty, "empty type path"))?;
            match seg.ident.to_string().as_str() {
                "i32"  => Ok(LuaType::I32),
                "u32"  => Ok(LuaType::U32),
                "f32"  => Ok(LuaType::F32),
                "bool" => Ok(LuaType::Bool),
                other  => Err(syn::Error::new_spanned(ty,
                    format!("unsupported lua_export type `{other}`; \
                             use i32, u32, f32, bool, or ()"))),
            }
        }
        _ => Err(syn::Error::new_spanned(ty,
            "unsupported lua_export type; use i32, u32, f32, bool, or ()")),
    }
}

/* -------------------------------------------------------------------------
 * Code generation helpers
 * ------------------------------------------------------------------------- */

fn c_void() -> TokenStream2 {
    quote! { *mut ::core::ffi::c_void }
}

fn c_int() -> TokenStream2 {
    quote! { ::core::ffi::c_int }
}

/// Extern "C" declarations + expression to read argument `stack_idx` from L.
fn gen_arg_get(lt: LuaType, stack_idx: i32, l: &syn::Ident) -> (TokenStream2, TokenStream2) {
    let cv = c_void();
    let ci = c_int();
    let idx = Literal::i32_unsuffixed(stack_idx);
    match lt {
        LuaType::I32 => (
            quote! { extern "C" {
                fn luaL_checkinteger(l: #cv, arg: #ci) -> i32;
            } },
            quote! { unsafe { luaL_checkinteger(#l, #idx) } },
        ),
        LuaType::U32 => (
            quote! { extern "C" {
                fn luaL_checkinteger(l: #cv, arg: #ci) -> i32;
            } },
            quote! { unsafe { luaL_checkinteger(#l, #idx) } as u32 },
        ),
        LuaType::F32 => (
            quote! { extern "C" {
                fn luaL_checknumber(l: #cv, arg: #ci) -> f32;
            } },
            quote! { unsafe { luaL_checknumber(#l, #idx) } },
        ),
        LuaType::Bool => (
            quote! { extern "C" {
                fn lua_toboolean(l: #cv, idx: #ci) -> #ci;
            } },
            quote! { unsafe { lua_toboolean(#l, #idx) } != 0 },
        ),
        LuaType::Void => unreachable!("void cannot appear as argument type"),
    }
}

/// Extern "C" declaration + statement to push return value `r` to L.
fn gen_ret_push(lt: LuaType, l: &syn::Ident) -> (TokenStream2, TokenStream2) {
    let cv = c_void();
    match lt {
        LuaType::Void => (quote! {}, quote! {}),
        LuaType::I32  => (
            quote! { extern "C" { fn lua_pushinteger(l: #cv, n: i32); } },
            quote! { unsafe { lua_pushinteger(#l, _r) }; },
        ),
        LuaType::U32  => (
            quote! { extern "C" { fn lua_pushinteger(l: #cv, n: i32); } },
            quote! { unsafe { lua_pushinteger(#l, _r as i32) }; },
        ),
        LuaType::F32  => (
            quote! { extern "C" { fn lua_pushnumber(l: #cv, n: f32); } },
            quote! { unsafe { lua_pushnumber(#l, _r) }; },
        ),
        LuaType::Bool => (
            quote! { extern "C" { fn lua_pushboolean(l: #cv, b: ::core::ffi::c_int); } },
            quote! { unsafe { lua_pushboolean(#l, if _r { 1 } else { 0 }) }; },
        ),
    }
}

/// Generate a byte-literal array of exactly N bytes, zero-padded.
fn fixed_bytes(s: &str, n: usize) -> TokenStream2 {
    let mut bytes = vec![0u8; n];
    let src = s.as_bytes();
    let copy_len = src.len().min(n.saturating_sub(1));
    bytes[..copy_len].copy_from_slice(&src[..copy_len]);
    let elems = bytes.iter().map(|&b| {
        let lit = Literal::u8_unsuffixed(b);
        quote! { #lit }
    });
    quote! { [#(#elems),*] }
}

/* -------------------------------------------------------------------------
 * The attribute macro
 * ------------------------------------------------------------------------- */

#[proc_macro_attribute]
pub fn lua_export(attr: TokenStream, item: TokenStream) -> TokenStream {
    let args   = parse_macro_input!(attr as MacroArgs);
    let func   = parse_macro_input!(item as ItemFn);
    match lua_export_impl(args, func) {
        Ok(ts)  => ts.into(),
        Err(e)  => e.to_compile_error().into(),
    }
}

fn lua_export_impl(args: MacroArgs, func: ItemFn) -> syn::Result<TokenStream2> {
    let fn_name   = &func.sig.ident;
    let fn_name_s = fn_name.to_string();
    let lua_name  = args.lua_name.unwrap_or_else(|| fn_name_s.clone());

    /* ------------------------------------------------------------------
     * Parse return type
     * ------------------------------------------------------------------ */
    let ret_lua = match &func.sig.output {
        ReturnType::Default           => LuaType::Void,
        ReturnType::Type(_, box_ty) => parse_lua_type(box_ty)?,
    };

    /* ------------------------------------------------------------------
     * Parse arguments (max 4, no self/receiver)
     * ------------------------------------------------------------------ */
    let inputs = &func.sig.inputs;
    if inputs.len() > 4 {
        return Err(syn::Error::new_spanned(inputs,
            "lua_export supports at most 4 arguments"));
    }

    let mut arg_lua_types: Vec<LuaType> = Vec::new();
    let mut arg_names: Vec<syn::Ident>  = Vec::new();
    for (i, input) in inputs.iter().enumerate() {
        match input {
            FnArg::Receiver(r) => {
                return Err(syn::Error::new_spanned(r,
                    "lua_export does not support self receivers"));
            }
            FnArg::Typed(PatType { ty, .. }) => {
                arg_lua_types.push(parse_lua_type(ty)?);
                arg_names.push(format_ident!("_a{}", i));
            }
        }
    }

    /* ------------------------------------------------------------------
     * Identifiers for generated symbols
     * ------------------------------------------------------------------ */
    let l = format_ident!("_l");
    let export_fn  = format_ident!("__lua_export_{}", fn_name);
    let reg_fn     = format_ident!("__lua_reg_{}", fn_name);
    let regptr_sym = format_ident!("__LUA_REGPTR_{}", fn_name_s.to_uppercase());
    let export_sym = format_ident!("__LUA_EXPORT_{}", fn_name_s.to_uppercase());

    let wrap_sym_s = format!("__lua_export_{}", fn_name);
    let lua_name_null = format!("{lua_name}\0");

    /* ------------------------------------------------------------------
     * Generate argument extraction (extern decls + let bindings)
     * ------------------------------------------------------------------ */
    let cv = c_void();
    let ci = c_int();

    let mut arg_extern_decls = TokenStream2::new();
    let mut arg_bindings     = TokenStream2::new();
    for (i, (lt, aname)) in arg_lua_types.iter().zip(arg_names.iter()).enumerate() {
        let (decl, expr) = gen_arg_get(*lt, (i + 1) as i32, &l);
        arg_extern_decls.extend(decl);
        arg_bindings.extend(quote! { let #aname = #expr; });
    }

    /* ------------------------------------------------------------------
     * Generate return value push
     * ------------------------------------------------------------------ */
    let (ret_extern_decl, ret_push) = gen_ret_push(ret_lua, &l);
    let nresults: u8 = if ret_lua == LuaType::Void { 0 } else { 1 };
    let nresults_lit = Literal::u8_unsuffixed(nresults);

    /* ------------------------------------------------------------------
     * Generate the call expression (assemble arg names)
     * ------------------------------------------------------------------ */
    let call_expr = quote! { #fn_name(#(#arg_names),*) };

    let call_and_push = if ret_lua == LuaType::Void {
        quote! { #call_expr; }
    } else {
        quote! { let _r = #call_expr; #ret_push }
    };

    /* ------------------------------------------------------------------
     * .lua_exports metadata
     * ------------------------------------------------------------------ */
    let mut arg_type_codes = [0u8; 4];
    for (i, lt) in arg_lua_types.iter().enumerate() {
        arg_type_codes[i] = lt.type_code();
    }
    let nargs_lit     = Literal::u8_unsuffixed(arg_lua_types.len() as u8);
    let ret_type_lit  = Literal::u8_unsuffixed(ret_lua.type_code());
    let at0 = Literal::u8_unsuffixed(arg_type_codes[0]);
    let at1 = Literal::u8_unsuffixed(arg_type_codes[1]);
    let at2 = Literal::u8_unsuffixed(arg_type_codes[2]);
    let at3 = Literal::u8_unsuffixed(arg_type_codes[3]);

    let lua_name_bytes  = fixed_bytes(&lua_name,     32);
    let fn_sym_bytes    = fixed_bytes(&fn_name_s,    64);
    let wrap_sym_bytes  = fixed_bytes(&wrap_sym_s,   64);

    let null_name: &[u8] = lua_name_null.as_bytes();
    let null_name_lit: Vec<_> = null_name.iter().map(|&b| Literal::u8_unsuffixed(b)).collect();

    /* ------------------------------------------------------------------
     * Emit everything
     * ------------------------------------------------------------------ */
    Ok(quote! {
        #func

        #[no_mangle]
        unsafe extern "C" fn #export_fn(#l: #cv) -> #ci {
            #arg_extern_decls
            #ret_extern_decl
            #arg_bindings
            #call_and_push
            #nresults_lit as #ci
        }

        #[no_mangle]
        unsafe extern "C" fn #reg_fn(#l: #cv) {
            extern "C" {
                fn lua_pushcclosure(#l: #cv,
                    f: unsafe extern "C" fn(#cv) -> #ci,
                    n: ::core::ffi::c_int);
                fn lua_setglobal(#l: #cv, k: *const u8);
            }
            unsafe {
                lua_pushcclosure(#l, #export_fn, 0);
                lua_setglobal(#l, [#(#null_name_lit),*].as_ptr());
            }
        }

        #[link_section = ".lua_regtab"]
        #[used]
        static #regptr_sym: unsafe extern "C" fn(#cv) = #reg_fn;

        #[link_section = ".lua_exports"]
        #[used]
        static #export_sym: ::blyt::lua::BlytLuaExportEntry = ::blyt::lua::BlytLuaExportEntry {
            lua_name:  #lua_name_bytes,
            fn_sym:    #fn_sym_bytes,
            wrap_sym:  #wrap_sym_bytes,
            nargs:     #nargs_lit,
            arg_types: [#at0, #at1, #at2, #at3],
            ret_type:  #ret_type_lit,
            pad:       [0, 0],
        };
    })
}
