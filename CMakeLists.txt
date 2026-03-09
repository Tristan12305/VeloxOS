cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(X86_64_ELF_GCC NAMES x86_64-elf-gcc)
    if(X86_64_ELF_GCC)
        set(CMAKE_C_COMPILER "${X86_64_ELF_GCC}")
    endif()
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(os_kernel LANGUAGES C ASM_NASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)

add_executable(kernel.elf
    main.c
    boot/boot.c
    include/printk.c
    kernel/panic.c
    mm/paging.c
    mm/paging.S
    mm/pmm.c
    arch/x86_64/cpu/apic.c
    arch/x86_64/gdt.c
    arch/x86_64/gdt.S
    arch/x86_64/idt.c
    arch/x86_64/idt.S
    arch/x86_64/isr_handler.c
    arch/x86_64/cpu/acpi.c
    arch/x86_64/cpu/madt.c

)

set_source_files_properties(
    mm/paging.S
    arch/x86_64/gdt.S
    arch/x86_64/idt.S
    PROPERTIES LANGUAGE ASM_NASM
)

target_include_directories(kernel.elf PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(kernel.elf PRIVATE c_std_11)

target_compile_options(kernel.elf PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
    $<$<COMPILE_LANGUAGE:C>:-fno-stack-protector>
    $<$<COMPILE_LANGUAGE:C>:-fno-pic>
    $<$<COMPILE_LANGUAGE:C>:-fno-pie>
    $<$<COMPILE_LANGUAGE:C>:-m64>
    $<$<COMPILE_LANGUAGE:C>:-mcmodel=kernel>
    $<$<COMPILE_LANGUAGE:C>:-mno-red-zone>
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:C>:-Wextra>
)

target_link_options(kernel.elf PRIVATE
    -nostdlib
    -static
    -no-pie
    -Wl,-T,${CMAKE_SOURCE_DIR}/linker.ld
    -Wl,-z,max-page-size=0x1000
    -Wl,--build-id=none
)

target_link_libraries(kernel.elf PRIVATE gcc)

option(BUILD_LIMINE_IMAGE "Enable the 'image' target (bootable Limine ISO)" ON)
set(LIMINE_DIR "${CMAKE_SOURCE_DIR}/limine" CACHE PATH "Directory containing Limine binary files")
set(LIMINE_CONF "${CMAKE_SOURCE_DIR}/limine.conf" CACHE FILEPATH "Path to limine.conf")
set(LIMINE_TOOL "" CACHE FILEPATH "Path to limine executable (supports 'bios-install')")

if(BUILD_LIMINE_IMAGE)
    if(NOT LIMINE_TOOL)
        find_program(LIMINE_TOOL NAMES limine PATHS "${LIMINE_DIR}" "${LIMINE_DIR}/bin")
    endif()
    find_program(XORRISO_EXECUTABLE NAMES xorriso)

    set(LIMINE_BIOS_SYS "${LIMINE_DIR}/limine-bios.sys")
    set(LIMINE_BIOS_CD "${LIMINE_DIR}/limine-bios-cd.bin")
    set(LIMINE_UEFI_CD "${LIMINE_DIR}/limine-uefi-cd.bin")
    set(LIMINE_EFI_X64 "${LIMINE_DIR}/BOOTX64.EFI")

    set(_missing "")
    foreach(_path
        "${LIMINE_CONF}"
        "${LIMINE_BIOS_SYS}"
        "${LIMINE_BIOS_CD}"
        "${LIMINE_UEFI_CD}"
        "${LIMINE_EFI_X64}"
    )
        if(NOT EXISTS "${_path}")
            list(APPEND _missing "${_path}")
        endif()
    endforeach()

    if(NOT XORRISO_EXECUTABLE)
        list(APPEND _missing "xorriso (program)")
    endif()
    if(NOT LIMINE_TOOL)
        list(APPEND _missing "limine (program)")
    endif()

    if(_missing)
        string(JOIN "\n  - " _missing_list ${_missing})
        message(WARNING
            "Limine image target disabled; missing required files/tools:\n"
            "  - ${_missing_list}\n"
            "Set LIMINE_DIR/LIMINE_TOOL/LIMINE_CONF and reconfigure to enable the 'image' target."
        )
    else()
        set(ISO_ROOT "${CMAKE_BINARY_DIR}/iso_root")
        set(ISO_PATH "${CMAKE_BINARY_DIR}/os.iso")

        add_custom_target(image
            DEPENDS kernel.elf
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${ISO_ROOT}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ISO_ROOT}/boot/limine"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ISO_ROOT}/EFI/BOOT"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:kernel.elf>" "${ISO_ROOT}/boot/kernel.elf"
            COMMAND ${CMAKE_COMMAND} -E copy "${LIMINE_CONF}" "${ISO_ROOT}/boot/limine/limine.conf"
            COMMAND ${CMAKE_COMMAND} -E copy "${LIMINE_BIOS_SYS}" "${ISO_ROOT}/boot/limine/limine-bios.sys"
            COMMAND ${CMAKE_COMMAND} -E copy "${LIMINE_BIOS_CD}" "${ISO_ROOT}/boot/limine/limine-bios-cd.bin"
            COMMAND ${CMAKE_COMMAND} -E copy "${LIMINE_UEFI_CD}" "${ISO_ROOT}/boot/limine/limine-uefi-cd.bin"
            COMMAND ${CMAKE_COMMAND} -E copy "${LIMINE_EFI_X64}" "${ISO_ROOT}/EFI/BOOT/BOOTX64.EFI"
            COMMAND ${XORRISO_EXECUTABLE} -as mkisofs
                -b boot/limine/limine-bios-cd.bin
                -no-emul-boot
                -boot-load-size 4
                -boot-info-table
                --efi-boot boot/limine/limine-uefi-cd.bin
                -efi-boot-part
                --efi-boot-image
                --protective-msdos-label
                "${ISO_ROOT}"
                -o "${ISO_PATH}"
            COMMAND ${LIMINE_TOOL} bios-install "${ISO_PATH}"
            COMMENT "Building Limine bootable ISO: ${ISO_PATH}"
            VERBATIM
        )
    endif()
endif()
