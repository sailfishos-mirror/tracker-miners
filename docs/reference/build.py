#!/usr/bin/env python3
# Tracker website build script.
#
# Copyright 2020, Sam Thursfield <sam@afuera.me.uk>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.


import bs4

import argparse
import contextlib
import logging
import pathlib
import shutil
import subprocess
import sys
import tempfile

log = logging.getLogger('build.py')

website_root = pathlib.Path(__file__).parent
docs_root = website_root.parent
source_root = docs_root.parent

asciidoc = shutil.which('asciidoc')
mkdocs = shutil.which('mkdocs')
xmlto = shutil.which('xmlto')


def argument_parser():
    parser = argparse.ArgumentParser(
        description="Tracker website build script")
    parser.add_argument('--output', required=True, metavar='OUTPUT',
                        help="Output directory")
    parser.add_argument('--debug', dest='debug', action='store_true',
                        help="Enable detailed logging to stderr")
    parser.add_argument('--man-pages', nargs='+', required=True,
                        help="List of Asciidoc manual page source")
    return parser


class Manpages():
    def run(self, command):
        command = [str(c) for c in command]
        log.debug("Running: %s", ' '.join(command))
        subprocess.run(command, check=True)

    def generate_manpage_xml(self, in_path, out_path):
        """Generate a docbook XML file for an Asciidoc manpage source file."""
        self.run([asciidoc, '--backend', 'docbook', '--doctype', 'manpage',
                 '--out-file', out_path, in_path])

    def generate_manpage_html(self, in_path, out_path):
        """Generate a HTML page from a docbook XML file"""
        self.run([xmlto, 'xhtml-nochunks', '-o', out_path, in_path])

    def generate_toplevel_markdown(self, in_path, out_path, html_files):
        """Generate the main commandline.md page."""

        includes = []
        for path in sorted(html_files):
            parser = bs4.BeautifulSoup(path.read_text(), 'html.parser')

            title = parser.title.text
            if title and title.startswith('tracker3-'):
                title = 'tracker3 ' + title[9:]

            body = parser.body.contents[0]

            includes.append("## %s\n\n%s\n---\n" % (title, body))

        text = in_path.read_text()
        text = text.format(
            includes='\n'.join(includes)
        )
        out_path.write_text(text)


@contextlib.contextmanager
def tmpdir():
    path = pathlib.Path(tempfile.mkdtemp())
    log.debug("Created temporary directory %s", path)
    try:
        yield path
    finally:
        log.debug("Removed temporary directory %s", path)
        shutil.rmtree(path)


def main():
    args = argument_parser().parse_args()
    output_path = pathlib.Path(args.output)

    if args.debug:
        logging.basicConfig(stream=sys.stderr, level=logging.DEBUG)
    else:
        logging.basicConfig(stream=sys.stderr, level=logging.INFO)

    if output_path.exists():
        raise RuntimeError(f"Output path '{output_path}' already exists.")

    if asciidoc is None:
        raise RuntimeError("The 'asciidoc' tool is required.")
    if xmlto is None:
        raise RuntimeError("The 'xmlto' tool is required.")

    log.info("Generating online man pages")
    with tmpdir() as workdir:
        manpages = Manpages()

        htmldir = website_root.joinpath('docs')
        htmlfiles = []

        for man_page in args.man_pages:
            asciidocpath = pathlib.Path(man_page)
            xmlpath = workdir.joinpath(asciidocpath.stem + '.xml')

            manpages.generate_manpage_xml(asciidocpath, xmlpath)
            manpages.generate_manpage_html(xmlpath, htmldir)

            htmlpath = htmldir.joinpath(xmlpath.with_suffix('.html').name)
            htmlfiles.append(htmlpath)

        template_in = website_root.joinpath('commandline.md.in')
        template_out = website_root.joinpath('commandline.md')

        manpages.generate_toplevel_markdown(template_in, template_out, htmlfiles)


    log.info("Building website")
    mkdocs_config = docs_root.joinpath('mkdocs.yml')
    subprocess.run([mkdocs, 'build', '--config-file', mkdocs_config,
                    '--site-dir', output_path.absolute()],
                   check=True)

    log.info("Documentation available in %s/ directory.", args.output)


try:
    main()
except (RuntimeError, subprocess.CalledProcessError) as e:
    sys.stderr.write(f"ERROR: {e}\n")
    sys.exit(1)
