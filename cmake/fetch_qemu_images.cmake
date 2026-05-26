# Downloads QEMU kernel, rootfs, and SSH key from a GitHub release into
# QEMU_IMAGES_DIR (passed via -D when invoked from the fetch_qemu_images target).
#
# Direct usage:
#   cmake -DQEMU_IMAGES_DIR=/path/to/qemu-images \
#         [-DBLYT_QEMU_IMAGE_URL=https://github.com/blyt-tech/blyt-qemu-images/releases/download/v2] \
#         -P cmake/fetch_qemu_images.cmake

if(NOT DEFINED BLYT_QEMU_IMAGE_URL)
  set(BLYT_QEMU_IMAGE_URL
      "https://github.com/blyt-tech/blyt-qemu-images/releases/download/v2")
endif()

if(NOT DEFINED QEMU_IMAGES_DIR)
  message(FATAL_ERROR "QEMU_IMAGES_DIR must be set")
endif()

file(MAKE_DIRECTORY "${QEMU_IMAGES_DIR}")

foreach(asset IN ITEMS kernel rootfs.qcow2 id_ed25519)
  set(dest "${QEMU_IMAGES_DIR}/${asset}")
  if(EXISTS "${dest}")
    message(STATUS "fetch_qemu_images: ${asset} already present — skip")
  else()
    message(STATUS "fetch_qemu_images: downloading ${asset} …")
    file(DOWNLOAD
      "${BLYT_QEMU_IMAGE_URL}/${asset}"
      "${dest}"
      SHOW_PROGRESS
      STATUS dl_status
    )
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
      list(GET dl_status 1 dl_error)
      file(REMOVE "${dest}")
      message(FATAL_ERROR "fetch_qemu_images: failed to download ${asset}: ${dl_error}")
    endif()
    if(asset STREQUAL "id_ed25519")
      file(CHMOD "${dest}" PERMISSIONS OWNER_READ OWNER_WRITE)
    endif()
  endif()
endforeach()

message(STATUS "fetch_qemu_images: done — images in ${QEMU_IMAGES_DIR}")
