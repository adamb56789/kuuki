cmake_minimum_required(VERSION 3.28)
project(kuuki C)

set(CMAKE_C_STANDARD 11)

add_executable(get_latest_data get_latest_data.c)

find_package(CURL REQUIRED)
target_link_libraries(get_latest_data PRIVATE CURL::libcurl)

target_compile_options(get_latest_data PRIVATE
        -Werror=attribute-warning
)

function(package_lambda target)
    set(package_dir "${CMAKE_BINARY_DIR}/packages/${target}")

    add_custom_command(
            TARGET ${target}
            POST_BUILD

            COMMAND ${CMAKE_COMMAND} -E make_directory
            "${package_dir}"

            COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:${target}>"
            "${package_dir}/bootstrap"

            COMMAND chmod +x "${package_dir}/bootstrap"

            COMMAND zip -j -9
            "${CMAKE_BINARY_DIR}/${target}.zip"
            "${package_dir}/bootstrap"

            COMMENT "Packaging Lambda ${target}"
    )
endfunction()

package_lambda(get_latest_data)