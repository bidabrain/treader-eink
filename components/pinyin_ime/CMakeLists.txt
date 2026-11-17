idf_component_register(
    SRCS "pinyin_ime.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES heap
)

# 静态分析构建：idf.py -B build-analyze -DTREADER_ANALYZE=1 build
# Static-analysis build: idf.py -B build-analyze -DTREADER_ANALYZE=1 build
if(TREADER_ANALYZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -fanalyzer -fstack-usage -Wno-error)
endif()
