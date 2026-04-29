
include(CPM)
include(CheckCXXSourceCompiles)

# C++ library feature test macros may be found here:
# https://en.cppreference.com/cpp/feature_test#Library_features

# Check for cpp_feature_macro support, set variable ${our_name} if present:
# HAVE_CPP_LIB_GENERATOR if std::generator support is present: 
function(has_cpp_feature cpp_feature_macro our_name)
  message(STATUS "Checking for C++ feature: ${cpp_feature_macro} / ${our_name}")

  set(src "
      #include <version>
      #ifndef ${cpp_feature_macro}
       #error feature is not present: ${cpp_feature_macro}
      #endif
      int main() {}
    ")

   try_compile(${our_name}
      SOURCE_FROM_CONTENT src-check-for-${cpp_feature_macro}.cpp "${src}"
   )

  set(${our_name} ${${our_name}} PARENT_SCOPE)

  message(STATUS "C++ feature available: ${cpp_feature_macro} / ${our_name}")
endfunction()

# TChecking ry to get a C++ feature using CPM if not already installed:
# (The extra arguments are passed right to CPM.)
function(obtain_cpp_feature cpp_feature_macro feature_tag_name our_name)
 has_cpp_feature(${cpp_feature_macro} ${our_name})

 # Nothing to do if it's already ok:
 if(feature_found)
     set(${our_name} TRUE PARENT_SCOPE)
     return()
 endif()

 # Try to install; pass any remaining parameters to CPM and see 
 # if it can sort out the situation:
 CPMAddPackage(NAME ${feature_tag_name} ${ARGN})
 set(${our_name} TRUE PARENT_SCOPE)

 message(STATUS "C++ feature obtained: ${cpp_feature_macro} / ${our_name}")
endfunction()

# Try to obtain C++ feature if not available; fail if couldn't
# obtain (the extra arguments are passed right to CPM); the tag name will
# be the same as uppercase "feature tag name":
function(require_cpp_feature cpp_feature_macro feature_tag_name)
 string(TOUPPER "${feature_tag_name}" local_feature_tag_name)
 obtain_cpp_feature(${cpp_feature_macro} ${feature_tag_name} ${local_feature_tag_name} ${ARGN})

 if(NOT ${local_feature_tag_name})
  message(FATAL_ERROR "C++ feature ${cpp_feature_macro} / ${feature_tag_name} / ${local_feature_tag_name} could not be made available.")
 endif()
endfunction()

