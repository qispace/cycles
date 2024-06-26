# SPDX-License-Identifier: Apache-2.0
# Copyright 2011-2022 Blender Foundation

#####################################################################
# Cycles standalone executable
#####################################################################

set(INC
  ..
)
set(INC_SYS
)

set(LIB
  cycles_device
  cycles_kernel
  cycles_scene
  cycles_session
  cycles_bvh
  cycles_subd
  cycles_graph
  cycles_util
)

if(WITH_ALEMBIC)
  add_definitions(-DWITH_ALEMBIC)
  list(APPEND INC_SYS
    ${ALEMBIC_INCLUDE_DIRS}
  )
  list(APPEND LIB
    ${ALEMBIC_LIBRARIES}
  )
endif()

if(WITH_CYCLES_OSL)
  list(APPEND LIB cycles_kernel_osl)
endif()

if(CYCLES_STANDALONE_REPOSITORY)
  list(APPEND LIB extern_sky)
else()
  list(APPEND LIB bf_intern_sky)
endif()

if(WITH_CYCLES_STANDALONE AND WITH_CYCLES_STANDALONE_GUI)
  list(APPEND INC_SYS
    ${Epoxy_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
  )
  list(APPEND LIB ${Epoxy_LIBRARIES} ${SDL2_LIBRARIES})
endif()

if(WITH_USD)
  # Silence warning from USD headers using deprecated TBB header.
  add_definitions(
    -D__TBB_show_deprecation_message_atomic_H
    -D__TBB_show_deprecation_message_task_H
  )

  list(APPEND INC_SYS
    ${USD_INCLUDE_DIRS}
  )
  list(APPEND LIB
    cycles_hydra
    ${USD_LIBRARIES}
  )
endif()

cycles_external_libraries_append(LIB)

# Common configuration.

include_directories(${INC})
include_directories(SYSTEM ${INC_SYS})

# Application build targets

if(WITH_CYCLES_STANDALONE)
  set(SRC
    cycles_standalone.cpp
    cycles_xml.cpp
    cycles_xml.h
    oiio_output_driver.cpp
    oiio_output_driver.h
	cycles_engine.cpp
	cycles_engine_scene.cpp
	cycles_engine.h
	offline_cycles.cpp
	offline_cycles.h
	interactive_cycles.cpp
	interactive_cycles_opengl.cpp
	interactive_cycles_scene.cpp
	interactive_cycles.h
	image_memory_oiio.cpp
	image_memory_oiio.h
  )

  if(WITH_CYCLES_STANDALONE_GUI)
    list(APPEND SRC
      opengl/display_driver.cpp
      opengl/display_driver.h
      opengl/shader.cpp
      opengl/shader.h
      opengl/window.cpp
      opengl/window.h
    )
  endif()
  
  # fix the output path
  #set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../x64")
  #set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../x64")
  #set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../x64")
  
  # Export as DLL
  add_definitions(-DUNICODE -D_UNICODE)
  add_definitions(-DCYCLES_LIB_EXPORTS)
  #set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
	
  add_library(cycles SHARED ${SRC} ${INC} ${INC_SYS})
  unset(SRC)
  
  target_link_libraries(cycles PUBLIC ${LIB})

  if(APPLE)
    if(WITH_CYCLES_STANDALONE_GUI)
      set_property(TARGET cycles APPEND_STRING PROPERTY LINK_FLAGS
        " -framework Cocoa -framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework ForceFeedback -framework CoreVideo")
    endif()
  endif()

  if(UNIX AND NOT APPLE)
    set_target_properties(cycles PROPERTIES INSTALL_RPATH $ORIGIN/lib)
  endif()

  if(CYCLES_STANDALONE_REPOSITORY)
    cycles_install_libraries(cycles)
  endif()

  if(WITH_USD AND USD_LIBRARY_DIR)
    install(DIRECTORY
      ${USD_LIBRARY_DIR}/usd
      DESTINATION ${CMAKE_INSTALL_PREFIX}
    )

    install(DIRECTORY
      ${USD_LIBRARY_DIR}/../plugin/usd
      DESTINATION ${CMAKE_INSTALL_PREFIX}
    )
  endif()

  install(PROGRAMS
    $<TARGET_FILE:cycles>
    DESTINATION ${CMAKE_INSTALL_PREFIX})
endif()
