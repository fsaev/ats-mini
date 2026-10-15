cmake_minimum_required(VERSION 3.16)

# Libraries live in git submodules under components/. Components that are
# in the top level directory of a submodule are found automatically, the
# ones inside a multi-component repository need to be listed explicitly.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-protocols/components/mdns
    ${CMAKE_CURRENT_LIST_DIR}/components/idf-extra-components/qrcode
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ats-mini)
