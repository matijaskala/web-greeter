#!/usr/bin/python3
# -*- coding: utf-8 -*-
#
#  greeter.py
#
#  Copyright © 2017 Antergos
#
#  This file is part of Web Greeter.
#
#  Web Greeter is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 3 of the License, or
#  (at your option) any later version.
#
#  Web Greeter is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  The following additional terms are in effect as per Section 7 of the license:
#
#  The preservation of all legal notices and author attributions in
#  the material or in the Appropriate Legal Notices displayed
#  by works containing it is required.
#
#  You should have received a copy of the GNU General Public License
#  along with Web Greeter; If not, see <http://www.gnu.org/licenses/>.

# Standard Lib
import os
from typing import (
    ClassVar,
    Type,
)

# 3rd-Party Libs
from PyQt5.QtCore import Qt
from PyQt5.QtCore import QFile
from PyQt5.QtCore import QUrl
from PyQt5.QtWidgets import QApplication
from PyQt5.QtWidgets import QMainWindow
from PyQt5.QtWebChannel import QWebChannel
from PyQt5.QtWebEngineWidgets import QWebEngineScript
from PyQt5.QtWebEngineWidgets import QWebEngineView

# This Application
import resources
from bridge import (
    Greeter,
    ThemeUtils,
)


BASE_DIR = os.path.dirname(os.path.realpath(__file__))
CONFIG_FILE = os.path.join(BASE_DIR, 'whither.yml')


class WebGreeter(QApplication):
    greeter = None         # type: ClassVar[BridgeObj]
    theme_utils = None     # type: ClassVar[BridgeObj]

    def __init__(self, *args, **kwargs) -> None:
        super().__init__([])

        self._main_window = QMainWindow()
        #self._main_window.setAttribute(Qt.WA_DeleteOnClose)
        self._main_window.setWindowTitle('Web Greeter for LightDM')
        #self._main_window.setWindowFlags(self._main_window.windowFlags() | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.MaximizeUsingFullscreenGeometryHint)
        self._main_window.setCursor(Qt.ArrowCursor)
        #self._main_window.setWindowState(Qt.WindowMaximized)
        self.view = QWebEngineView(parent=self._main_window)
        self.view.show()
        self._main_window.setCentralWidget(self.view)
        self._main_window.show()

        self.greeter = Greeter('/usr/share/web-greeter/themes')
        self.theme_utils = ThemeUtils(self.greeter)

        self._web_container_bridge_objects = (self.greeter, self.theme_utils)
        self.channel = QWebChannel(self.view.page())
        self.view.page().setWebChannel(self.channel)
        registered_objects = self.channel.registeredObjects()
        for obj in self._web_container_bridge_objects:
            if obj not in registered_objects:
                self.channel.registerObject(obj._name, obj)

        self._create_webengine_script(':/qwebchannel/qwebchannel.js', 'QWebChannel API')
        self._create_webengine_script(':/_greeter/js/bundle.js', 'Web Greeter Bundle')
        self.view.page().scripts().insert(':/_greeter/js/bundle.js', 'Web Greeter Bundle')
        self.load_theme()

    @staticmethod
    def _create_webengine_script(path: QUrl, name: str) -> QWebEngineScript:
        script = QWebEngineScript()
        script_file = QFile(path)

        if script_file.open(QFile.ReadOnly):
            script_string = str(script_file.readAll(), 'utf-8')

            script.setInjectionPoint(QWebEngineScript.DocumentCreation)
            script.setName(name)
            script.setWorldId(QWebEngineScript.MainWorld)
            script.setSourceCode(script_string)

        return script

    def _before_web_container_init(self):
        self.get_and_apply_user_config()

    def load_theme(self):
        theme_url = '/{0}/{1}/index.html'.format(self.config.themes_dir, self.config.greeter.theme)
        self._web_container.load(theme_url)


if __name__ == '__main__':
    greeter = WebGreeter()

    greeter.run()
