if(NOT DEFINED MODULE_SOURCE)
  message(FATAL_ERROR "MODULE_SOURCE is required")
endif()

file(READ "${MODULE_SOURCE}" source)

foreach(required
        "apiVersion"
        "deprecatedVendorRequests"
        "vendor_set_error"
        "workspace_unavailable"
        "unsupported_operation"
        "motion_profile_not_found"
        "motion_shot_not_found")
  if(NOT source MATCHES "${required}")
    message(FATAL_ERROR "Vendor API contract marker missing: ${required}")
  endif()
endforeach()

message(STATUS "Vendor API contract markers passed")
