use proc_macro::TokenStream;
use proc_macro2::{Literal, TokenStream as TokenStream2};
use quote::{format_ident, quote};
use syn::{
    parse::{Parse, ParseStream},
    parse_macro_input, FnArg, ItemFn, LitStr, PatType, ReturnType, Token, Type, TypePath,
};

/* -------------------------------------------------------------------------
 * Attribute arguments: #[lua_export], #[lua_export(name = "foo")],
 *                      #[lua_export(module = "mylib")], optionally with a
 *                      trailing `raw` flag: #[lua_export(module = "m", raw)]
 * ------------------------------------------------------------------------- */

struct MacroArgs {
    lua_name: Option<String>,
    module:   Option<String>,
    raw:      bool,
}

impl Parse for MacroArgs {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let mut args = MacroArgs { lua_name: None, module: None, raw: false };
        while !input.is_empty() {
            let key: syn::Ident = input.parse()?;
            if key == "raw" {
                args.raw = true;
            } else if key == "name" || key == "module" {
                let _: Token![=] = input.parse()?;
                let lit: LitStr = input.parse()?;
                if key == "name" {
                    args.lua_name = Some(lit.value());
                } else {
                    args.module = Some(lit.value());
                }
            } else {
                return Err(syn::Error::new(key.span(),
                    "expected `name = \"...\"`, `module = \"...\"`, or `raw`"));
            }
            if input.is_empty() {
                break;
            }
            let _: Token![,] = input.parse()?;
        }
        Ok(args)
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

fn lua_type_to_rust(lt: LuaType) -> TokenStream2 {
    match lt {
        LuaType::I32  => quote! { i32 },
        LuaType::U32  => quote! { u32 },
        LuaType::F32  => quote! { f32 },
        LuaType::Bool => quote! { bool },
        LuaType::Void => quote! { () },
    }
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
    let module_s  = args.module;
    // For module exports: C symbol = "module_fn", Lua name = "module.fn"
    // For global exports: C symbol = fn_name (or explicit name), Lua name = same
    let (sym_name_s, lua_name) = if let Some(ref m) = module_s {
        (format!("{}_{}", m, fn_name_s), format!("{}.{}", m, fn_name_s))
    } else {
        let n = args.lua_name.unwrap_or_else(|| fn_name_s.clone());
        (n.clone(), n)
    };

    /* Raw exports (ADR-0130) take an entirely different shape; see below. */
    if args.raw {
        return lua_export_raw_impl(func, &sym_name_s, &lua_name, module_s.as_deref());
    }

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
     * sym_name_s = fn_name for globals, "module_fn" for module exports
     * ------------------------------------------------------------------ */
    let sym_upper = sym_name_s.to_uppercase();
    let l = format_ident!("_l");
    let export_fn  = format_ident!("__lua_export_{}", sym_name_s);
    let regtab_sym = format_ident!("__LUA_REGTAB_{}", sym_upper);
    let export_sym = format_ident!("__LUA_EXPORT_{}", sym_upper);
    let regtab_fn_name_sym = format_ident!("__LUA_REGTAB_FN_{}", sym_upper);

    let wrap_sym_s = format!("__lua_export_{}", sym_name_s);
    // Shim function: for module exports, the C symbol is "module_fn" (distinct
    // from the Rust fn name).  For globals, we keep "__blyt_fnsym_fn" so the
    // shim has a different symbol than the user's Rust function and the call
    // inside the shim body resolves to the Rust function, not itself.
    let (fnsym_fn, fnsym_s) = if module_s.is_some() {
        (format_ident!("{}", sym_name_s), sym_name_s.clone())
    } else {
        let s = format!("__blyt_fnsym_{}", sym_name_s);
        (format_ident!("{}", s), s)
    };

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

    /* Shim: #[no_mangle] pub extern "C" fn __blyt_fnsym_NAME(args) -> ret { NAME(args) }
     * Gives the WASM host a stable, dynsym-visible symbol to look up for trampolines. */
    let shim_arg_decls: Vec<TokenStream2> = arg_names.iter().zip(arg_lua_types.iter())
        .map(|(aname, lt)| { let ty = lua_type_to_rust(*lt); quote! { #aname: #ty } })
        .collect();
    let shim_ret_ty = if ret_lua != LuaType::Void {
        let ty = lua_type_to_rust(ret_lua);
        quote! { -> #ty }
    } else {
        quote! {}
    };

    let lua_name_bytes = fixed_bytes(&lua_name,   32);
    let fn_sym_bytes   = fixed_bytes(&fnsym_s,    64);
    let wrap_sym_bytes = fixed_bytes(&wrap_sym_s, 64);

    /* Null-terminated lua_fn_name string for the .lua_regtab struct.
     * For globals this is just fn_name; for modules it is the bare fn name
     * (without the "module." prefix, which is stored in module_name). */
    let regtab_fn_name_s: &str = if module_s.is_some() {
        &fn_name_s
    } else {
        &lua_name
    };
    let fn_name_null_bytes: Vec<_> = {
        let mut v: Vec<u8> = regtab_fn_name_s.bytes().collect();
        v.push(0);
        v.iter().map(|&b| Literal::u8_unsuffixed(b)).collect()
    };
    let fn_name_null_len = regtab_fn_name_s.len() + 1;

    /* module_name static + pointer expression for the .lua_regtab entry. */
    let (module_name_static, module_name_ptr_expr) = if let Some(ref m) = module_s {
        let mod_sym = format_ident!("__LUA_REGTAB_MOD_{}", sym_upper);
        let mod_null_bytes: Vec<_> = {
            let mut v: Vec<u8> = m.bytes().collect();
            v.push(0);
            v.iter().map(|&b| Literal::u8_unsuffixed(b)).collect()
        };
        let mod_null_len = m.len() + 1;
        let stat = quote! {
            static #mod_sym: [u8; #mod_null_len] = [#(#mod_null_bytes),*];
        };
        let ptr = quote! {
            &#mod_sym as *const [u8; #mod_null_len] as *const u8
        };
        (stat, ptr)
    } else {
        (quote! {}, quote! { ::core::ptr::null::<u8>() })
    };

    /* ------------------------------------------------------------------
     * Emit everything
     * ------------------------------------------------------------------ */
    Ok(quote! {
        #func

        #[no_mangle]
        pub extern "C" fn #fnsym_fn(#(#shim_arg_decls),*) #shim_ret_ty {
            #fn_name(#(#arg_names),*)
        }

        #[no_mangle]
        unsafe extern "C" fn #export_fn(#l: #cv) -> #ci {
            #arg_extern_decls
            #ret_extern_decl
            #arg_bindings
            #call_and_push
            #nresults_lit as #ci
        }

        #module_name_static
        static #regtab_fn_name_sym: [u8; #fn_name_null_len] = [#(#fn_name_null_bytes),*];

        #[link_section = ".lua_regtab"]
        #[used]
        static #regtab_sym: ::blyt::lua::BlytLuaRegtabEntry = ::blyt::lua::BlytLuaRegtabEntry {
            module_name: #module_name_ptr_expr,
            lua_fn_name: &#regtab_fn_name_sym as *const [u8; #fn_name_null_len] as *const u8,
            wrapper: #export_fn,
        };

        #[link_section = ".lua_exports"]
        #[used]
        static #export_sym: ::blyt::lua::BlytLuaExportEntry = ::blyt::lua::BlytLuaExportEntry {
            lua_name:  #lua_name_bytes,
            fn_sym:    #fn_sym_bytes,
            wrap_sym:  #wrap_sym_bytes,
            nargs:     #nargs_lit,
            arg_types: [#at0, #at1, #at2, #at3],
            ret_type:  #ret_type_lit,
            flags:     0,
            pad:       0,
        };
    })
}

/* -------------------------------------------------------------------------
 * Raw exports (ADR-0130) — Rust counterpart of BLYT_LUA_MODULE_EXPORT_RAW.
 *
 * The user writes the lua_CFunction-shaped wrapper body directly against the
 * restricted Lua C API (blyt::lua::capi) — strings, tables, any number of
 * arguments, multiple returns:
 *
 *   #[lua_export(module = "greeting", raw)]
 *   fn log(l: LuaState) {                       // or: -> i32 (result count)
 *       unsafe { ... luaL_checklstring(l, 1, ...) ... }
 *   }
 *
 * The same body runs on every target: real Lua C API on rv32; ECALL-bridged
 * on WASM (flags = LUA_EXPORT_FLAG_BRIDGED).  A `()` return means "no
 * results pushed" (the generated wrapper returns 0); an `i32` return is the
 * Lua result count, passed through.
 * ------------------------------------------------------------------------- */

fn lua_export_raw_impl(
    func: ItemFn,
    sym_name_s: &str,
    lua_name: &str,
    module_s: Option<&str>,
) -> syn::Result<TokenStream2> {
    let fn_name   = &func.sig.ident;
    let fn_name_s = fn_name.to_string();

    /* Signature: exactly one argument (the Lua state) and () or i32 return. */
    let inputs = &func.sig.inputs;
    if inputs.len() != 1 || matches!(inputs.first(), Some(FnArg::Receiver(_))) {
        return Err(syn::Error::new_spanned(inputs,
            "raw lua_export functions take exactly one argument: \
             the Lua state (blyt::lua::LuaState)"));
    }
    let returns_count = match &func.sig.output {
        ReturnType::Default => false,
        ReturnType::Type(_, box_ty) => match &**box_ty {
            Type::Tuple(t) if t.elems.is_empty() => false,
            Type::Path(TypePath { path, .. })
                if path.segments.last().is_some_and(|s| s.ident == "i32") => true,
            other => {
                return Err(syn::Error::new_spanned(other,
                    "raw lua_export functions return () or i32 \
                     (the Lua result count)"));
            }
        },
    };

    let cv = c_void();
    let ci = c_int();
    let l  = format_ident!("_l");

    let sym_upper  = sym_name_s.to_uppercase();
    let raw_sym_s  = format!("__blyt_lua_raw_{}", sym_name_s);
    let raw_fn     = format_ident!("{}", raw_sym_s);
    let regtab_sym = format_ident!("__LUA_REGTAB_{}", sym_upper);
    let export_sym = format_ident!("__LUA_EXPORT_{}", sym_upper);
    let regtab_fn_name_sym = format_ident!("__LUA_REGTAB_FN_{}", sym_upper);

    let call = if returns_count {
        quote! { #fn_name(#l) as #ci }
    } else {
        quote! { #fn_name(#l); 0 as #ci }
    };

    /* .lua_exports: fn_sym == wrap_sym == the raw wrapper; arg/ret metadata
     * is unused on the bridged path (the wrapper reads the stack itself). */
    let lua_name_bytes = fixed_bytes(lua_name,   32);
    let raw_sym_bytes  = fixed_bytes(&raw_sym_s, 64);

    /* .lua_regtab fn name: bare name within the module, or the global name. */
    let regtab_fn_name_s: &str = if module_s.is_some() { &fn_name_s } else { lua_name };
    let fn_name_null_bytes: Vec<_> = {
        let mut v: Vec<u8> = regtab_fn_name_s.bytes().collect();
        v.push(0);
        v.iter().map(|&b| Literal::u8_unsuffixed(b)).collect()
    };
    let fn_name_null_len = regtab_fn_name_s.len() + 1;

    let (module_name_static, module_name_ptr_expr) = if let Some(m) = module_s {
        let mod_sym = format_ident!("__LUA_REGTAB_MOD_{}", sym_upper);
        let mod_null_bytes: Vec<_> = {
            let mut v: Vec<u8> = m.bytes().collect();
            v.push(0);
            v.iter().map(|&b| Literal::u8_unsuffixed(b)).collect()
        };
        let mod_null_len = m.len() + 1;
        let stat = quote! {
            static #mod_sym: [u8; #mod_null_len] = [#(#mod_null_bytes),*];
        };
        let ptr = quote! {
            &#mod_sym as *const [u8; #mod_null_len] as *const u8
        };
        (stat, ptr)
    } else {
        (quote! {}, quote! { ::core::ptr::null::<u8>() })
    };

    Ok(quote! {
        #func

        #[no_mangle]
        unsafe extern "C" fn #raw_fn(#l: #cv) -> #ci {
            #call
        }

        #module_name_static
        static #regtab_fn_name_sym: [u8; #fn_name_null_len] = [#(#fn_name_null_bytes),*];

        #[link_section = ".lua_regtab"]
        #[used]
        static #regtab_sym: ::blyt::lua::BlytLuaRegtabEntry = ::blyt::lua::BlytLuaRegtabEntry {
            module_name: #module_name_ptr_expr,
            lua_fn_name: &#regtab_fn_name_sym as *const [u8; #fn_name_null_len] as *const u8,
            wrapper: #raw_fn,
        };

        #[link_section = ".lua_exports"]
        #[used]
        static #export_sym: ::blyt::lua::BlytLuaExportEntry = ::blyt::lua::BlytLuaExportEntry {
            lua_name:  #lua_name_bytes,
            fn_sym:    #raw_sym_bytes,
            wrap_sym:  #raw_sym_bytes,
            nargs:     0,
            arg_types: [0, 0, 0, 0],
            ret_type:  0,
            flags:     ::blyt::lua::LUA_EXPORT_FLAG_BRIDGED,
            pad:       0,
        };
    })
}
