#!/usr/bin/env python3
"""Check that desktop, settings, resource and RPM identities match the build."""

import argparse
import configparser
import re
import struct
from pathlib import Path
from urllib.parse import urlparse
from xml.etree import ElementTree as ET


def check(root, app_id, version):
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    require(re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?', version),
            'Use a numeric release version with an optional prerelease suffix')
    data = root / 'data'
    desktop = configparser.ConfigParser(interpolation=None)
    desktop.read(data / (app_id + '.desktop'))
    require(desktop.has_section('Desktop Entry'), 'Matching desktop entry is missing')
    require(desktop['Desktop Entry'].get('Icon') == app_id, 'Desktop icon ID differs from application ID')
    require(desktop['Desktop Entry'].get('Exec') == 'pirwa', 'Desktop entry must launch pirwa')

    meta = ET.parse(data / (app_id + '.metainfo.xml')).getroot()
    require(meta.findtext('id') == app_id, 'AppStream component ID differs from application ID')
    require(meta.findtext("launchable[@type='desktop-id']") == app_id + '.desktop',
            'AppStream launchable differs from desktop filename')
    release = meta.find('releases/release')
    require(release is not None and release.get('version') == version,
            'Newest AppStream release must match the Meson version')
    kinds = {'snapshot', 'development'} if '-' in version else {'stable'}
    require(release.get('type', 'stable') in kinds,
            'AppStream release kind must distinguish snapshots from stable releases')

    schema = ET.parse(data / (app_id + '.gschema.xml')).getroot().find('schema')
    require(schema is not None and schema.get('id') == app_id,
            'GSettings schema ID differs from application ID')
    resources = ET.parse(data / 'pirwa.gresource.xml').getroot().find('gresource')
    require(resources is not None and resources.get('prefix') == '/' + app_id.replace('.', '/') + '/icons/scalable/apps',
            'Icon resource prefix differs from application ID')
    icons = {entry.text for entry in resources.findall('file')}
    require(icons == {app_id + '.svg', app_id + '-symbolic.svg'}, 'Both application icons must be embedded')
    require(all((data / icon).is_file() for icon in icons), 'An embedded application icon is missing')

    spec = (root / 'packaging/pirwa.spec').read_text()
    def tag(name):
        match = re.search(r'^' + name + r':\s*(\S+)\s*$', spec, re.MULTILINE)
        require(match is not None, 'RPM ' + name + ' is missing')
        return match[1]
    require(tag('Version') == version.split('-', 1)[0], 'RPM version differs from Meson release version')
    require(tag('Source0') == '%{name}-%{pirwa_source_version}.tar.xz',
            'RPM source archive must use the selected source version')
    require('%autosetup -n %{name}-%{pirwa_source_version}' in spec,
            'RPM extraction directory differs from source archive prefix')
    require(meta.findtext("url[@type='homepage']") == tag('URL'),
            'AppStream homepage and RPM project URL differ')
    screenshots = meta.findall('screenshots/screenshot/image')
    require(screenshots, 'At least one real application screenshot is required')
    for image in screenshots:
        url = urlparse(image.text or '')
        require(url.scheme == 'https', 'Screenshot URLs must use HTTPS')
        path = data / 'screenshots' / Path(url.path).name
        with path.open('rb') as stream:
            header = stream.read(24)
        require(header[:8] == b'\x89PNG\r\n\x1a\n' and len(header) == 24, 'Screenshot must be a PNG')
        width, height = struct.unpack('>II', header[16:24])
        require((width, height) == (int(image.get('width', 0)), int(image.get('height', 0))),
                'Screenshot dimensions differ from AppStream metadata')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--app-id', required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    try:
        check(args.source_root, args.app_id, args.version)
    except (ValueError, OSError, ET.ParseError, configparser.Error) as error:
        parser.exit(1, f'Metadata mismatch: {error}\n')
    print(f'Metadata agrees: {args.app_id} {args.version}')


if __name__ == '__main__':
    main()
