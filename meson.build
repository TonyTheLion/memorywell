#project
project('memorywell', 'c',
	version : '0.1.8',
	default_options : [ 'c_std=gnu11' ])
description = 'nonblocking circular buffer'

#sanity
assert(meson.project_name() == 'memorywell', 'Incorrect project name')


#
#	build options by type
#
# debug : all the pedantic things!
if get_option('buildtype') == 'debug'
	add_project_arguments([ '-DDEBUG' ],
				language : 'c')

# debugoptimized : daily worker build; basically 'release' but with symbols
elif get_option('buildtype') == 'debugoptimized'
	add_project_arguments([ '-march=native', '-mtune=native' ],
				language : 'c')

# release : optimize this! ship this!
elif get_option('buildtype') == 'release'
	add_project_arguments([ '-DNDEBUG', '-march=native', '-mtune=native', '-Ofast' ],
				language : 'c')

# plain : NO flags and an old architecture;
#+	use this to cross check the effect of flags on the other build types.
elif get_option('buildtype') == 'plain'
	add_project_arguments([ '-DNDEBUG', '-march=core2' ],
				language : 'c')
endif


# deps
# deps are either statically or dynamically incorporated;
#+	see meson_options.txt for the default value, which can be overridden
#+	with a -Ddep_type=["shared"||"static"] flag when running meson.

dep_static = false
_dep = get_option('dep_type')
if _dep == 'static'
	dep_static = true
endif

# Insert deps here
# Note that subproject expected to export as '[name]_dep_[shared||static]'
nonlibc = dependency('nonlibc', static: dep_static, version : '>=0.1.8',
		fallback : ['nonlibc', 'nonlibc_dep_' + _dep ], required : true)


# All deps in a single arg. Use THIS ONE in compile calls
deps = [ nonlibc ]


#build
inc = include_directories('include')
subdir('include')
subdir('src')
subdir('test')
subdir('benchmark')
