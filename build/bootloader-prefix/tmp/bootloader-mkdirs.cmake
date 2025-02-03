# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/roman-slack/esp32project/esp-idf/components/bootloader/subproject"
  "/home/roman-slack/esp32project/demo_lock_project/build/bootloader"
  "/home/roman-slack/esp32project/demo_lock_project/build/bootloader-prefix"
  "/home/roman-slack/esp32project/demo_lock_project/build/bootloader-prefix/tmp"
  "/home/roman-slack/esp32project/demo_lock_project/build/bootloader-prefix/src/bootloader-stamp"
  "/home/roman-slack/esp32project/demo_lock_project/build/bootloader-prefix/src"
  "/home/roman-slack/esp32project/demo_lock_project/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/roman-slack/esp32project/demo_lock_project/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/roman-slack/esp32project/demo_lock_project/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
