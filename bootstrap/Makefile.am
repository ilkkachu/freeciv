## Process this file with automake to produce Makefile.in

## Automake should distribute config.rpath automatically - and new
## versions probably do. We still support versions that do not, so
## it's in the list below.
## It should also install it automatically when running autogen.sh.
EXTRA_DIST =    fcgui.in		\
		fcser.in		\
		fcruledit.in		\
		fc_gitrev_gen.h.tmpl	\
		generate_gitrev.sh	\
		generate_langstat.sh	\
		langstat_freeciv.txt	\
		langstat_nations.txt	\
		langstat_ruledit.txt	\
		freeciv.desktop.in	\
		freeciv-gtk2.desktop.in \
		freeciv-gtk3.22.desktop.in \
		freeciv-sdl.desktop.in \
		freeciv-sdl2.desktop.in \
		freeciv-qt.desktop.in \
		freeciv-server.desktop.in	\
		freeciv-mp-gtk2.desktop.in	\
		freeciv-mp-gtk3.desktop.in	\
		freeciv-mp-qt.desktop.in	\
		freeciv-ruledit.desktop.in	\
		freeciv-gtk2.appdata.xml.in \
		freeciv-gtk3.appdata.xml.in \
		freeciv-gtk3.22.appdata.xml.in \
		freeciv-sdl.appdata.xml.in \
		freeciv-sdl2.appdata.xml.in \
		freeciv-qt.appdata.xml.in \
		freeciv-server.appdata.xml.in \
		freeciv-mp-gtk2.appdata.xml.in \
		freeciv-mp-gtk3.appdata.xml.in \
		freeciv-mp-qt.appdata.xml.in \
		freeciv-ruledit.appdata.xml.in \
		config.rpath		\
		Makerules.mk		\
		langnames.txt

langstat_freeciv.txt:
	"$(top_srcdir)/bootstrap/generate_langstat.sh" freeciv "$(top_srcdir)" "$(top_builddir)"

langstat_nations.txt:
	"$(top_srcdir)/bootstrap/generate_langstat.sh" nations "$(top_srcdir)" "$(top_builddir)"

langstat_ruledit.txt:
	"$(top_srcdir)/bootstrap/generate_langstat.sh" ruledit "$(top_srcdir)" "$(top_builddir)"

.PHONY: langstat_freeciv.txt langstat_nations.txt langstat_ruledit.txt

MAINTAINERCLEANFILES = langstat_freeciv.txt langstat_nations.txt langstat_ruledit.txt
