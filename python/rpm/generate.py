# -*- coding: utf-8 -*-
#
# Copyright 2016-2017 Per Øyvind Karlsen <proyvind@moondrake.org>
#
# This program is free software. It may be redistributed and/or modified under
# the terms of the LGPL version 2.1 (or later).
#
# Runtime python package generator for use with embedded python interpreter

import distutils.dist
from distutils.command.bdist_rpm import bdist_rpm
from distutils.command import sdist
from distutils.sysconfig import get_config_var
from distutils.filelist import FileList
import distutils.core
from tempfile import NamedTemporaryFile

import os, sys
from types import *
from glob import glob
import rpm
from subprocess import Popen, PIPE
from shutil import rmtree
from time import gmtime, strftime
from locale import resetlocale, setlocale, LC_TIME

import json
try:
    from urllib.request import urlopen
    from urllib.error import HTTPError, URLError
except ImportError:
    from urllib2 import urlopen, HTTPError, URLError

def _show_warning(message, category=Warning, filename=None, lineno=0, file=None, line=None):
    return

# swiped from Patchwork Buildroot development and adapted to our purpose
def _pypi_source_url(pkg_name, version, ext):
    '''Get package release metadata from PyPI'''

    metadata_url = 'https://pypi.python.org/pypi/{pkg}/json'.format(pkg=pkg_name)
    try:
        pkg_json = urlopen(metadata_url).read().decode()
        ver_metadata = json.loads(pkg_json)['releases'][version]
    except:
        ver_metadata = None

    br_pypi_url = None
    full_pkg_name = '{name}-{version}.{ext}'.format(name=pkg_name,
                                                version=version,
                                                ext=ext)

    if ver_metadata:
        for download_url in ver_metadata:
            if 'bdist' in download_url['packagetype']:
                continue

            if download_url['url'].endswith(full_pkg_name):
                br_pypi_url = download_url['url']+"#"+download_url['md5_digest']
                break

    return (full_pkg_name, br_pypi_url)

class _bdist_rpm(bdist_rpm):
    def _make_spec_file(self, url, changelog):
        """Generate the text of an RPM spec file and return it as a
        list of strings (one per line).
        """
        sdist = self.reinitialize_command('sdist')

        sdist.warn = _show_warning
        sdist.finalize_options()
        sdist.filelist = FileList()
        sdist.get_file_list()
        manifest = sdist.filelist.files

        build_py = sdist.get_finalized_command('build_py')
        name = self.distribution.get_name()
        version = self.distribution.get_version().replace('-','_')
        release = self.release.replace('-','_')
        summary = self.distribution.get_description().strip().strip('.')

        spec_file = [
            '%define\tmodule\t'+name,
            ]
        module = '%{module}'

        spec_file.extend([
            '',
            'Name:\t\tpython-' + module,
            'Version:\t' + version,
            'Release:\t' + release,
            'Summary:\t' + summary,
            'Source0:\t' + url,
            ])
        # XXX yuck! this filename is available from the "sdist" command,
        # but only after it has run: and we create the spec file before
        # running "sdist", in case of --spec-only.
        """
        if suffix == "zip":
            spec_file[-1] += suffix
        elif sdist.formats and 'xztar' in sdist.formats:
            spec_file[-1] += 'tar.xz'
        elif sdist.formats and 'bztar' in sdist.formats:
            spec_file[-1] += 'tar.bz2'
        else:
            spec_file[-1] += 'tar.gz'
            """

        license = self.distribution.get_license()
        if license == "UNKNOWN":
                classifiers = self.distribution.get_classifiers()
                for classifier in classifiers:
                        values = classifier.split(" :: ")
                        if values[0] == "License":
                                license = values[-1]
        license.replace("GPL ", "GPLv").strip()
        spec_file.extend([
            'License:\t' + license,
            'Group:\t\tDevelopment/Python',])# + self.group,])
        if self.distribution.get_url() != 'UNKNOWN':
            spec_file.append('Url:\t\t' + self.distribution.get_url())

        doc_names = ['README', 'CHANGES','ChangeLog', 'NEWS', 'THANKS',
                'HISTORY', 'AUTHORS', 'BUGS', 'ReleaseNotes', 'DISCLAIMER',
                'TODO', 'TROUBLESHOOTING', 'IDEAS', 'HACKING', 'WISHLIST',
                'CREDITS', 'PROJECTS', 'LEGAL', 'KNOWN_BUGS',
                'MISSING_FEATURES', 'FAQ', 'ANNOUNCE', 'FEATURES', 'WHATSNEW']
        license_names = ['LICENSE', 'COPYRIGHT', 'COPYING']
        common_licenses = glob('/usr/share/common-licenses/*')
        for i in range(len(common_licenses)):
            common_licenses[i] = os.path.basename(common_licenses[i])
        doc_names.extend(license_names)
        doc_suffixes = ('.doc', '.htm', '.txt', '.pdf', '.odt')

        #print(self.distribution.get_command_list())
        self.doc_files = []
        self.license_files = []
        all_files = []
        if self.distribution.data_files:
            all_files.extend(self.distribution.data_files)
        if manifest:
            all_files.extend(manifest)
        if all_files:
            for data_file in all_files:
                done = False
                for doc_name in doc_names:
                    if doc_name.lower() in data_file.lower():
                        # skip licenses already shipped with common-licenses package
                        if doc_name in license_names:
                            if license not in common_licenses:
                                self.license_files.append(data_file)
                                all_files.remove(data_file)
                                done = True
                                break
                        if data_file in doc_names or data_file.endswith(".md"):
                            self.doc_files.append(data_file)
                            all_files.remove(data_file)
                            done = True
                            break
                if done:
                    continue
                for doc_suffix in doc_suffixes:
                    ext = os.path.splitext(data_file.lower())[1]
                    if ext.lower().startswith(doc_suffix.lower()):
                        #self.doc_files.append(data_file)
                        break
        if not self.force_arch:
            # noarch if no extension modules
            if not self.distribution.has_ext_modules():
                spec_file.append('BuildArch:\tnoarch')
        else:
            spec_file.append('BuildArch:\t%s' % self.force_arch)

        for field in ('Provides',
                      'Requires',
                      'Conflicts',
                      'Obsoletes',
                      ):
            val = getattr(self, field.lower())
            if type(val) is list:
                spec_file.append('%s: %s' % (field, " ".join(val)))
            elif val is not None:
                spec_file.append('%s: %s' % (field, val))

        build_requires = []
        if self.distribution.has_ext_modules():
            build_requires.append('python-devel')
        # Ugly, but should mostly work... :p
        if 'setuptools' in str(self.distribution.__dict__) or 'setuptools' in str(sdist.__dict__):
            build_requires.append('python-setuptools')
        if build_requires:
            spec_file.append('BuildRequires:\t' +
                             " ".join(build_requires))

        if self.build_requires:
            spec_file.append('BuildRequires:\t' +
                             " ".join(self.build_requires))

        spec_file.extend([
            '',
            '%description',
            self.distribution.get_long_description().strip()
            ])


        # insert contents of files

        # XXX this is kind of misleading: user-supplied options are files
        # that we open and interpolate into the spec file, but the defaults
        # are just text that we drop in as-is.  Hmmm.

        if 'test_suite' in self.distribution.__dict__ and self.distribution.test_suite:
            verify_script = "%{__python} setup.py test"
        else:
            verify_script = None

        script_options = [
            ('prep', 'prep_script', "%setup -qDTn %{module}-%{version}"),
            ('build', 'build_script', "%{__python} setup.py build"),
            ('install', 'install_script',
             ("%{__python} setup.py install "
              "--root=%{buildroot}")),
            ('check', 'verify_script', verify_script),
            ('pre', 'pre_install', None),
            ('post', 'post_install', None),
            ('preun', 'pre_uninstall', None),
            ('postun', 'post_uninstall', None),
        ]

        for (rpm_opt, attr, default) in script_options:
            # Insert contents of file referred to, if no file is referred to
            # use 'default' as contents of script
            val = getattr(self, attr)
            if val or default:
                spec_file.extend([
                    '',
                    '%' + rpm_opt,])
                if val:
                    f = open(val, 'r')
                    spec_file.extend(f.read().split(sep='\n'))
                    f.close()
                else:
                    spec_file.append(default)


        # files section
        spec_file.extend([
            '',
            '%files',
            ])

        for license_file in self.license_files:
            spec_file.append('%license ' + license_file)
        for doc_file in self.doc_files:
            spec_file.append('%doc ' + doc_file)

        if self.distribution.has_ext_modules():
            site_pkgs = '%{py_platsitedir}'
        else:
            site_pkgs = '%{py_puresitedir}'
        if self.distribution.has_scripts():
            for script in self.distribution.scripts:
                if type(script) == str:
                    spec_file.append(os.path.join('%{_bindir}', os.path.basename(script)))
        site_pkgs_files = []
        if self.distribution.data_files:
            for data_file in self.distribution.data_files:
                site_pkgs_files.append(os.path.join(site_pkgs, data_file))
        if 'entry_points' in self.distribution.__dict__ and self.distribution.entry_points:
            if type(self.distribution.entry_points) is dict:
                for entry_points in self.distribution.entry_points:
                    for entry_point in self.distribution.entry_points[entry_points]:
                        site_pkgs_files.append(os.path.join('%{_bindir}', os.path.basename(entry_point.split('=')[0])))
        if 'py_modules' in self.distribution.__dict__ and self.distribution.py_modules:
            for py_module in self.distribution.py_modules:
                py_module = py_module.replace('.', os.path.sep)
                site_pkgs_files.append(os.path.join(site_pkgs, py_module + '.py*'))
        if 'packages' in self.distribution.__dict__ and self.distribution.packages:
            for package in self.distribution.packages:
                package = package.replace('.', os.path.sep)
                spec_file.append('%dir ' + os.path.join(site_pkgs, package))
                site_pkgs_files.append(os.path.join(site_pkgs, package, '*.py*'))
        if self.distribution.has_ext_modules():
            for ext_module in self.distribution.ext_modules:
                ext_module = ext_module.name.replace('.', os.path.sep)
                site_pkgs_files.append(os.path.join(site_pkgs, ext_module + get_config_var('SO')))

        site_pkgs_files.sort()
        for f in site_pkgs_files:
            spec_file.append(f)

        spec_file.append(os.path.join(site_pkgs, name.replace('-', '_') + '*.egg-info'))

        if changelog:
            packager = rpm.expandMacro('%{?packager}%{?!packager:Unnamed Loser <foo@bar.cum>}')

            setlocale(LC_TIME, locale="C")
            spec_file.extend([
                    '',
                    '%changelog',
                    '* ' + strftime("%a %b %d %Y", gmtime()) + ' %s %s-%s' % (packager, version, release),
                    '- Initial release',])
            resetlocale()

        return spec_file

def pyspec(module, version, release="1", suffix="tar.gz", python=rpm.expandMacro("%{__python}"), changelog=False):
    os.chdir(rpm.expandMacro("%{_builddir}"))
    builddir = "%s-%s" % (module, version)
    if os.path.exists(builddir):
        rmtree(builddir)

    filename, url = _pypi_source_url(module, version, suffix)
    filepath = rpm.expandMacro("%{_sourcedir}/" + filename)
    if not (os.path.exists(filepath) and os.path.isfile(filepath) and os.stat(filepath).st_size != 0):
        download = urlopen(url)
        f = open(filepath, "wb")
        f.write(download.read())
        f.close()
    uncompress = Popen(rpm.expandMacro("%{uncompress: " + filepath + "}").split(), stdout=PIPE)
    if suffix == "zip":
        uncompress.wait()
    else:
        untar = Popen(rpm.expandMacro("%{__tar} -xo").split(), stdin=uncompress.stdout, stdout=PIPE)
        output = untar.communicate()[0]
        untar.wait()
    os.chdir(builddir)

    sys.argv = [rpm.expandMacro("%{__python}"), "setup.py"]
    try:
        dist = distutils.core.run_setup(sys.argv[1], stop_after="config")
    except RuntimeError:
        f = open(sys.argv[1], "r")
        setup_py = f.read()
        f.close()
        sys.argv[1] = "setup2.py"
        f = open(sys.argv[1], "w")

        f.write(setup_py.replace('"__main__"', '__name__'))
        f.close()
        dist = distutils.core.run_setup(sys.argv[1], stop_after="config")

    p = os.popen(python + " setup.py egg_info")
    egginfo = None
    for line in p.readlines():
        if ".egg-info/PKG-INFO" in line:
            for text in line.split():
                if os.path.exists(text):
                    egginfo = text
                    break
    p.close()

    distmeta = distutils.dist.DistributionMetadata(path=egginfo)
    distmeta.keywords = {"name" : module, "version" : version}
    dist.version = version
    dist.release = release
    dist.script_name = "setup.py"

    dist.metadata = distmeta

    for basename in dist.metadata._METHOD_BASENAMES:
        method_name = "get_" + basename
        setattr(dist, method_name, getattr(dist.metadata, method_name))

    specdist = _bdist_rpm(dist)
    specdist.spec_only = True
    specdist.initialize_options()
    specdist.finalize_options()
    specdist.finalize_package_data()
    specdist.distribution = dist
    specfile = specdist._make_spec_file(url, changelog=changelog)

    lines = "\n".join(specfile)
    tmp = NamedTemporaryFile(mode="w", suffix=".spec", prefix=module, dir=rpm.expandMacro("%{_tmppath}"), delete=True)
    tmp.write(lines)
    tmp.flush()
    spec = rpm.spec(tmp.name)
    tmp.close()
    output = ""
    lines = spec.parsed.split("\n")
    emptyheader = True
    for line in lines:
        if emptyheader:
            if len(line) > 0:
                emptyheader = False
            else:
                continue
        if "UNKNOWN" not in line:
            output += "%s\n" % line
    sys.stdout.write(output.rstrip("\n"))
