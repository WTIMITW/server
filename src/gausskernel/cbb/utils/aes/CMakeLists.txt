#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_aes_SRC)
set(TGT_util_aes_SRC ${CMAKE_CURRENT_SOURCE_DIR}/aes.cpp)

set(TGT_aes_INC 
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${ZLIB_INCLUDE_PATH}
    ${LIBOPENSSL_INCLUDE_PATH}
    ${LIBCURL_INCLUDE_PATH}
)

set(aes_DEF_OPTIONS ${MACRO_OPTIONS})
set(aes_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS})
set(aes_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_cbb_utils_aes TGT_aes_SRC TGT_aes_INC "${aes_DEF_OPTIONS}" "${aes_COMPILE_OPTIONS}" "${aes_LINK_OPTIONS}")
add_static_objtarget(utils_aes_aes TGT_util_aes_SRC TGT_aes_INC "${aes_DEF_OPTIONS}" "${aes_COMPILE_OPTIONS}" "${aes_LINK_OPTIONS}")
