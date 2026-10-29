# 文件作用：固定 qpdf 12.2.0 的进程内 PDF 加密依赖，禁止交叉编译误探测宿主机库。
# qpdf 的 JPEG 只用于已有 PDF 流过滤；产品已构建 OpenCV 时复用其同版 libjpeg-turbo。
# 独立 MaiAgent 才构建下面的同源、无 SIMD 后备目标，不额外下载或链接系统 JPEG。
function(mai_add_pdf_dependencies)
  set(pdf_jpeg_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/opencv/3rdparty/libjpeg-turbo")
  set(pdf_jpeg_binary "${CMAKE_CURRENT_BINARY_DIR}/pdf-jpeg-config")
  set(VERSION 3.1.2)
  set(JPEG_LIB_VERSION 70)
  set(LIBJPEG_TURBO_VERSION_NUMBER 3001002)
  set(BUILD "maichat-pdf-3.1.2")
  set(COPYRIGHT_YEAR "1991-2025")
  set(C_ARITH_CODING_SUPPORTED 1)
  set(D_ARITH_CODING_SUPPORTED 1)
  set(WITH_SIMD 0)
  set(SIZE_T "${CMAKE_SIZEOF_VOID_P}")
  set(THREAD_LOCAL "")
  set(HIDDEN "")
  if(MSVC)
    set(INLINE __forceinline)
    set(HAVE_INTRIN_H 1)
  else()
    set(INLINE __inline__)
  endif()
  configure_file("${pdf_jpeg_source}/jconfig.h.in" "${pdf_jpeg_binary}/jconfig.h")
  configure_file("${pdf_jpeg_source}/jconfigint.h.in" "${pdf_jpeg_binary}/jconfigint.h")
  configure_file("${pdf_jpeg_source}/jversion.h.in" "${pdf_jpeg_binary}/jversion.h")

  # 与仓库现有 OpenCV 的 libjpeg-turbo/CMakeLists 保持同一组 C 实现。
  # 后备目标 EXCLUDE_FROM_ALL，产品已有 libjpeg-turbo 时不会生成第二份归档。
  set(jpeg16_sources jcapistd.c jccolor.c jcdiffct.c jclossls.c jcmainct.c
    jcprepct.c jcsample.c jdapistd.c jdcolor.c jddiffct.c jdlossls.c jdmainct.c
    jdpostct.c jdsample.c jutils.c)
  set(jpeg12_sources ${jpeg16_sources} jccoefct.c jcdctmgr.c jdcoefct.c
    jddctmgr.c jdmerge.c jfdctfst.c jfdctint.c jidctflt.c jidctfst.c jidctint.c
    jidctred.c jquant1.c jquant2.c)
  set(jpeg_sources ${jpeg12_sources} jcapimin.c jchuff.c jcicc.c jcinit.c
    jclhuff.c jcmarker.c jcmaster.c jcomapi.c jcparam.c jcphuff.c jctrans.c
    jdapimin.c jdatadst.c jdatasrc.c jdhuff.c jdicc.c jdinput.c jdlhuff.c
    jdmarker.c jdmaster.c jdphuff.c jdtrans.c jerror.c jfdctflt.c jmemmgr.c
    jmemnobs.c jpeg_nbits.c jaricom.c jcarith.c jdarith.c)
  list(TRANSFORM jpeg16_sources PREPEND "${pdf_jpeg_source}/src/")
  list(TRANSFORM jpeg12_sources PREPEND "${pdf_jpeg_source}/src/")
  list(TRANSFORM jpeg_sources PREPEND "${pdf_jpeg_source}/src/")
  add_library(mai_pdf_jpeg12 OBJECT EXCLUDE_FROM_ALL ${jpeg12_sources})
  add_library(mai_pdf_jpeg16 OBJECT EXCLUDE_FROM_ALL ${jpeg16_sources})
  target_compile_definitions(mai_pdf_jpeg12 PRIVATE BITS_IN_JSAMPLE=12)
  target_compile_definitions(mai_pdf_jpeg16 PRIVATE BITS_IN_JSAMPLE=16)
  add_library(mai_pdf_jpeg_fallback STATIC EXCLUDE_FROM_ALL ${jpeg_sources}
    $<TARGET_OBJECTS:mai_pdf_jpeg12> $<TARGET_OBJECTS:mai_pdf_jpeg16>)
  foreach(target mai_pdf_jpeg12 mai_pdf_jpeg16 mai_pdf_jpeg_fallback)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target} PRIVATE "${pdf_jpeg_binary}" "${pdf_jpeg_source}/src")
    target_compile_definitions(${target} PRIVATE NO_GETENV NO_PUTENV)
  endforeach()

  # 显式列出本工程拥有的依赖目标，qpdf 不再走 pkg-config/find_library 猜路径。
  # 编译定义与 zlibstatic 配对，使 qpdf 也使用同一份私有 z_ 前缀。
  get_target_property(pdf_zlib_includes zlibstatic INTERFACE_INCLUDE_DIRECTORIES)
  if(MAI_OPENSSL_CRYPTO_TARGET)
    set(pdf_crypto_target "${MAI_OPENSSL_CRYPTO_TARGET}")
    set(pdf_crypto_includes "${MAI_OPENSSL_INCLUDE_DIR}")
  else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    set(pdf_crypto_target OpenSSL::Crypto)
    set(pdf_crypto_includes "${OPENSSL_INCLUDE_DIR}")
  endif()
  set(MAI_QPDF_DEP_INCLUDE_DIRS "${pdf_zlib_includes};${pdf_crypto_includes};${pdf_jpeg_binary};${pdf_jpeg_source}/src")
  set(MAI_QPDF_DEP_LIBRARIES
    "$<BUILD_INTERFACE:zlibstatic>"
    "$<BUILD_INTERFACE:${pdf_crypto_target}>"
    "$<BUILD_INTERFACE:$<IF:$<TARGET_EXISTS:libjpeg-turbo>,libjpeg-turbo,mai_pdf_jpeg_fallback>>")
  set(MAI_QPDF_EMBEDDED_DEPENDENCIES ON)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_STATIC_LIBS ON)
  set(BUILD_DOC OFF)
  set(BUILD_EXAMPLES OFF)
  set(GENERATE_AUTO_JOB OFF)
  set(WERROR OFF)
  set(USE_IMPLICIT_CRYPTO OFF)
  set(REQUIRE_CRYPTO_OPENSSL ON)
  set(ALLOW_CRYPTO_NATIVE OFF)
  set(DEFAULT_CRYPTO openssl)
  # Qt 的自动生成器不应扫描上游第三方源文件。
  set(CMAKE_AUTOMOC OFF)
  set(CMAKE_AUTOUIC OFF)
  set(CMAKE_AUTORCC OFF)
  add_subdirectory("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/qpdf"
    "${CMAKE_CURRENT_BINARY_DIR}/qpdf" EXCLUDE_FROM_ALL)
  set_target_properties(libqpdf_object PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(libqpdf_object PRIVATE POINTERHOLDER_TRANSITION=4
    Z_PREFIX z_errmsg=mai_z_errmsg)
endfunction()
