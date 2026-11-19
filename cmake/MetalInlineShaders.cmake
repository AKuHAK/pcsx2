# Склеивает Metal-шейдеры в один исходник для компиляции во время выполнения
# (MTLDevice newLibraryWithSource). Нужно, когда нет компилятора `xcrun metal`
# (только Command Line Tools, без Xcode).
#
# Вызов: cmake -DOUTPUT=<файл> -DSHADERS=<a.metal|b.metal|...> -P MetalInlineShaders.cmake
# (разделитель "|" или ";")
#
# Локальные #include "..." подставляются рекурсивно; файлы с #pragma once
# подставляются один раз на весь склеенный исходник. Системные #include <...>
# остаются как есть: их разрешает рантайм-компилятор Metal.

cmake_minimum_required(VERSION 3.16)

if(NOT OUTPUT OR NOT SHADERS)
	message(FATAL_ERROR "MetalInlineShaders: нужны OUTPUT и SHADERS")
endif()

string(REPLACE "|" ";" SHADERS "${SHADERS}")

set(_mtl_once "")
set(_mtl_out "")

function(_mtl_inline path)
	get_filename_component(path "${path}" REALPATH)
	if(NOT EXISTS "${path}")
		message(FATAL_ERROR "MetalInlineShaders: нет файла ${path}")
	endif()
	file(READ "${path}" text)
	if(text MATCHES "(^|\n)[ \t]*#[ \t]*pragma[ \t]+once")
		list(FIND _mtl_once "${path}" idx)
		if(NOT idx EQUAL -1)
			return()
		endif()
		list(APPEND _mtl_once "${path}")
		set(_mtl_once "${_mtl_once}" PARENT_SCOPE)
	endif()
	get_filename_component(dir "${path}" DIRECTORY)

	# Построчная обработка без потери ';' и '[' ']' (в Metal они на каждом шагу):
	# экранируем их перед разбиением на список и возвращаем обратно.
	string(REPLACE "\\" "\\\\" text "${text}")
	string(REPLACE "[" "<MTLLB>" text "${text}")
	string(REPLACE "]" "<MTLRB>" text "${text}")
	string(REPLACE ";" "<MTLSC>" text "${text}")
	string(REPLACE "\n" ";" lines "${text}")

	set(out "${_mtl_out}")
	foreach(line IN LISTS lines)
		string(REPLACE "<MTLLB>" "[" line "${line}")
		string(REPLACE "<MTLRB>" "]" line "${line}")
		string(REPLACE "<MTLSC>" ";" line "${line}")
		string(REPLACE "\\\\" "\\" line "${line}")
		if(line MATCHES "^[ \t]*#[ \t]*pragma[ \t]+once")
			continue()
		elseif(line MATCHES "^[ \t]*#[ \t]*include[ \t]+\"([^\"]+)\"")
			set(_mtl_out "${out}")
			_mtl_inline("${dir}/${CMAKE_MATCH_1}")
			set(out "${_mtl_out}")
		else()
			string(APPEND out "${line}\n")
		endif()
	endforeach()
	set(_mtl_out "${out}" PARENT_SCOPE)
	set(_mtl_once "${_mtl_once}" PARENT_SCOPE)
endfunction()

foreach(shader IN LISTS SHADERS)
	get_filename_component(name "${shader}" NAME)
	string(APPEND _mtl_out "// ---- ${name}\n")
	_mtl_inline("${shader}")
endforeach()

file(WRITE "${OUTPUT}.tmp" "${_mtl_out}")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
