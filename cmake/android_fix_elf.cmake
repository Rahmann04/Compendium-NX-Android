if(NOT EXISTS "${TARGET_FILE}")
    message(FATAL_ERROR "Library target tidak ditemukan: ${TARGET_FILE}")
endif()

execute_process(
    COMMAND "${READELF}" -d "${TARGET_FILE}"
    OUTPUT_VARIABLE INFO
    RESULT_VARIABLE READ_RESULT
)

if(NOT READ_RESULT EQUAL 0)
    message(FATAL_ERROR "Gagal membaca ELF.")
endif()

if(INFO MATCHES "\\[libfmt\\.so\\.12\\]")
    execute_process(
        COMMAND "${PATCHELF}" --replace-needed libfmt.so.12 libfmt.so "${TARGET_FILE}"
        RESULT_VARIABLE PATCH_RESULT
    )
    if(NOT PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Gagal memperbaiki dependensi fmt.")
    endif()
endif()

if(INFO MATCHES "\\[libSDL2\\.so\\.5\\]")
    execute_process(
        COMMAND "${PATCHELF}" --replace-needed libSDL2.so.5 libSDL2.so "${TARGET_FILE}"
        RESULT_VARIABLE PATCH_RESULT
    )
    if(NOT PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Gagal memperbaiki dependensi SDL2.")
    endif()
endif()

if(INFO MATCHES "\\((RPATH|RUNPATH)\\)")
    execute_process(
        COMMAND "${PATCHELF}" --remove-rpath "${TARGET_FILE}"
        RESULT_VARIABLE PATCH_RESULT
    )
    if(NOT PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Gagal menghapus RPATH.")
    endif()
endif()

execute_process(
    COMMAND "${READELF}" -d "${TARGET_FILE}"
    OUTPUT_VARIABLE FINAL_INFO
)

if(FINAL_INFO MATCHES "\\[libfmt\\.so\\.12\\]" OR
   FINAL_INFO MATCHES "\\[libSDL2\\.so\\.5\\]" OR
   FINAL_INFO MATCHES "\\((RPATH|RUNPATH)\\)")
    message(FATAL_ERROR "Verifikasi ELF gagal.")
endif()

message(STATUS "Android ELF dependency fix: BERHASIL")
