import sys, os, shutil
from subprocess import Popen, PIPE

env = Environment()

opts = Variables()
opts.Add(BoolVariable("mingwcross", "Cross-compile with mingw for Win32", 0))
opts.Add(BoolVariable("boost_static", "Statically link against Boost", 0))
Help(opts.GenerateHelpText(env))
opts.Update(env)

env.Append(CPPFLAGS=['-g', '-O3', '-std=c++17'])
platform = None
target = None

env.Append(LIBPATH=['.'])

# Homebrew paths on macOS (ARM and Intel)
if sys.platform == 'darwin':
	import subprocess
	brew_prefix = subprocess.check_output(['brew', '--prefix']).decode().strip()
	env.Append(CPPPATH=[brew_prefix + '/include'])
	env.Append(LIBPATH=[brew_prefix + '/lib'])

if sys.platform.startswith('linux'):
	platform = target = 'linux'
elif sys.platform == 'darwin':
	platform = target = 'osx'
elif sys.platform.startswith('win'):
	platform = target = 'windows'
else:
	print("Unknown platform")
	exit()

def run(cmd):
	process = Popen(cmd, stdout=PIPE, shell=True)
	r = process.communicate()[0].strip()
	process.wait()
	return r

gitversion = ''
if os.path.isdir('.git'):
	gitversion = run("git rev-parse --short=16 HEAD") \
	           + run("git status --porcelain 2>/dev/null | grep -e '^ [MADRC]' > /dev/null && echo '*'")

if env['mingwcross']:
	target = 'windows'
	env.Platform('cygwin')
	env['CC']  = 'i586-mingw32msvc-gcc'
	env['CXX'] = 'i586-mingw32msvc-g++'
	env['AR']  = 'i586-mingw32msvc-ar'
	env['RANLIB'] = 'i586-mingw32msvc-ranlib'


# In-tree static libraries (just libjson now)
libs = ['json']
libs = [env.File(env['LIBPREFIX']+i+env['LIBSUFFIX']) for i in libs]

# System libraries
libs += ['usb-1.0']

boostlibs = ['boost_regex', 'boost_thread']


boost_static = env['boost_static']

frameworks = []

if target == 'linux':
	env.Append(CPPFLAGS=['-fstack-protector-all', '-D_FORTIFY_SOURCE=2'], LINKFLAGS=['-fstack-protector-all'])
	boostlibs = [i+'-mt' for i in boostlibs]

	if boost_static:
		boost_lib = '/usr/lib/'
		boostlibs = [env.File(boost_lib+env['LIBPREFIX']+i+env['LIBSUFFIX']) for i in boostlibs]

	libs += ['udev', 'pthread', 'rt']

elif target == 'osx':
	env.Append(CPPFLAGS=['-fstack-protector-all', '-D_FORTIFY_SOURCE=2'], LINKFLAGS=['-fstack-protector-all'])
	# Modern homebrew Boost does not use -mt suffix
	if boost_static:
		boost_lib = '/usr/local/lib/'
		boostlibs = [env.File(boost_lib+env['LIBPREFIX']+i+env['LIBSUFFIX']) for i in boostlibs]

	libs += ['objc']
	frameworks = ['CoreFoundation', 'IOKit']

elif target == 'windows':
	if platform == 'windows':
		boost_dir='C:\\Program Files\\boost\\src\\'
		boost_lib=boost_dir+'stage\\lib\\'
		env.Append(CPPPATH = boost_dir)
	else:
		boost_lib = '../lib/win/boost/lib/'
		env.Append(CPPPATH = '../lib/win/boost/include')

	env.Append(
		tools=['mingw'],
		CPPFLAGS=["-D_WIN32_WINNT=0x0501", '-D BOOST_THREAD_USE_LIB', '-fno-strict-aliasing'],
		LIBPATH=[boost_lib, '.'],
		LINKFLAGS=['-Wl,-subsystem,windows', '-mwindows'],
	)

	boostlibs.remove('boost_thread')
	boostlibs.append('boost_thread_win32')
	if boost_static:
		boostlibs = [env.File(boost_lib+env['LIBPREFIX']+i+'-mt-s'+env['LIBSUFFIX']) for i in boostlibs]
	else:
		boostlibs = [i+'-mgw46-mt-1_48' for i in boostlibs]

	libs += ['ws2_32', 'mswsock']

json = env.Library('json',
	Glob('libjson/_internal/Source/*.cpp'),
	CCFLAGS = "-c -O3 -fexpensive-optimizations".split()
)


libs += boostlibs

t_env = env.Clone(CCFLAGS=['-Wall', '-g', '-O3', '-std=c++17'])

sources = Glob('*.cpp') +  Glob('streaming_device/*.cpp') + ['cee/cee.cpp', 'bootloader/bootloader.cpp']

# add GITVERSION define for version.cpp
objs = []
for s in sources:
	if str(s) != 'version.cpp':
		objs.append(t_env.Object(s))
	else:
		objs.append(t_env.Object(s, CPPDEFINES={'GITVERSION': gitversion}))

env.Program('nonolith-connect', objs, LIBS=libs, FRAMEWORKS=frameworks)
