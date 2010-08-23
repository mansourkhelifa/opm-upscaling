dnl -*- autoconf -*-
# Macros needed to find dune-upscaling and dependent libraries.  They are called by
# the macros in ${top_src_dir}/dependencies.m4, which is generated by
# "dunecontrol autogen"

# Additional checks needed to build dune-upscaling
# This macro should be invoked by every module which depends on dune-upscaling, as
# well as by dune-upscaling itself
AC_DEFUN([DUNE_UPSCALING_CHECKS])

# Additional checks needed to find dune-upscaling
# This macro should be invoked by every module which depends on dune-upscaling, but
# not by dune-upscaling itself
AC_DEFUN([DUNE_UPSCALING_CHECK_MODULE],
[
  DUNE_CHECK_MODULES([dune-upscaling],
                     [upscaling/SinglePhaseUpscaler.hpp])
])