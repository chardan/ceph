set(osdc_files
  Filer.cc
  ObjectCacher.cc
  Objecter.cc)

if(WITH_LIBRADOSSTRIPER)
  list(APPEND osdc_files Striper.cc)
endif()

add_library(osdc STATIC ${osdc_files})
if(WITH_LTTNG AND WITH_EVENTTRACE)
  add_dependencies(osdc eventtrace_tp)
endif()
