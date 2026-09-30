# Out-of-tree drivers.
#
# Each driver gets its own subdirectory and its own Kconfig symbol, so that
# an application only compiles what it actually enables.
#
# Add one line per driver as they are written, guarded by that driver's
# Kconfig symbol -- see the magnetometer PR for the pattern.
#
# (No drivers yet -- the first one arrives with the magnetometer PR.)
