#
# Makefile for the overlay filesystem.
#

obj-$(CONFIG_OVERLAY_FS) += overlay.o


overlay-y := super.o inode.o dir.o readdir.o copy_up.o
overlay-$(CONFIG_OVERLAY_FS_COMPAT) += compat.o
