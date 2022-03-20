
set(desSources des.cpp)
set(desHeaders des.h)
set(desFinalSources
  ${desSources}
  ${desHeaders}
)

add_pcsx2_lib(des "${desFinalSources}" "" "")
