#This is the main CMAKE for build bin.
AUX_SOURCE_DIRECTORY(${CMAKE_CURRENT_SOURCE_DIR} TGT_dss_SRC)

set(TGT_dss_INC 
    ${PROJECT_SRC_DIR}/include
)

set(dss_DEF_OPTIONS ${MACRO_OPTIONS})
set(dss_COMPILE_OPTIONS ${OPTIMIZE_OPTIONS} ${OS_OPTIONS} ${PROTECT_OPTIONS} ${WARNING_OPTIONS} ${BIN_SECURE_OPTIONS} ${CHECK_OPTIONS})
set(dss_LINK_OPTIONS ${BIN_LINK_OPTIONS})
add_static_objtarget(gausskernel_storage_dss TGT_dss_SRC TGT_dss_INC "${dss_DEF_OPTIONS}" "${dss_COMPILE_OPTIONS}" "${dss_LINK_OPTIONS}")
