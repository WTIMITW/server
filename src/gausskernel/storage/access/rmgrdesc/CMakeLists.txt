#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_rmgrdesc_SRC)
if("${ENABLE_MOT}" STREQUAL "OFF")
list(REMOVE_ITEM TGT_rmgrdesc_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/motdesc.cpp
)
endif()

set(TGT_rmgrdesc_INC 
    ${PROJECT_SRC_DIR}/gausskernel/cbb/communication
    ${PROJECT_SRC_DIR}/include/iprange
    ${PROJECT_SRC_DIR}/include/libcomm
    ${PROJECT_SRC_DIR}/include
    ${PROJECT_SRC_DIR}/lib/gstrace
    ${LIBCGROUP_INCLUDE_PATH}
    ${PROJECT_SRC_DIR}/include/libcomm
    ${ZLIB_INCLUDE_PATH}
    ${LIBCURL_INCLUDE_PATH} 
)

set(rmgrdesc_DEF_OPTIONS ${MACRO_OPTIONS})
set(rmgrdesc_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS})
set(rmgrdesc_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_storage_access_rmgrdesc TGT_rmgrdesc_SRC TGT_rmgrdesc_INC "${rmgrdesc_DEF_OPTIONS}" "${rmgrdesc_COMPILE_OPTIONS}" "${rmgrdesc_LINK_OPTIONS}")

