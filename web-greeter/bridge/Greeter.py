# -*- coding: utf-8 -*-
#
#  Greeter.py
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

# 3rd-Party Libs
import gi
gi.require_version('LightDM', '1')
from gi.repository import LightDM
from PyQt5.QtCore import (
    pyqtProperty,
    pyqtSignal,
    pyqtSlot,
    QObject,
    QTimer,
    QVariant,
)

# This Application
from . import (
    language_to_dict,
    layout_to_dict,
    session_to_dict,
    user_to_dict,
)


LightDMGreeter = LightDM.Greeter()
LightDMUsers = LightDM.UserList()


class Greeter(QObject):

    # LightDM.Greeter Signals
    authentication_complete = pyqtSignal()
    autologin_timer_expired = pyqtSignal()
    idle = pyqtSignal()
    reset = pyqtSignal()
    show_message = pyqtSignal(str, LightDM.MessageType, arguments=('text', 'type'))
    show_prompt = pyqtSignal(str, LightDM.PromptType, arguments=('text', 'type'))

    noop_signal = pyqtSignal()
    property_changed = pyqtSignal()

    def __init__(self, themes_dir, *args, **kwargs):
        super().__init__(parent=None)
        _name = 'LightDMGreeter'

        self._shared_data_directory = ''
        self._themes_directory = themes_dir

        LightDMGreeter.connect_to_daemon_sync()

        self._connect_signals()
        self._determine_shared_data_directory_path()

    def _determine_shared_data_directory_path(self):
        user = LightDMUsers.get_users()[0]
        user_data_dir = LightDMGreeter.ensure_shared_data_dir_sync(user.get_name())
        self._shared_data_directory = user_data_dir.rpartition('/')[0]

    def _connect_signals(self):
        LightDMGreeter.connect(
            'authentication-complete',
            lambda greeter: self._emit_signal(self.authentication_complete)
        )
        LightDMGreeter.connect(
            'autologin-timer-expired',
            lambda greeter: self._emit_signal(self.autologin_timer_expired)
        )

        LightDMGreeter.connect('idle', lambda greeter: self._emit_signal(self.idle))
        LightDMGreeter.connect('reset', lambda greeter: self._emit_signal(self.reset))

        LightDMGreeter.connect(
            'show-message',
            lambda greeter, msg, mtype: self._emit_signal(self.show_message, msg, mtype)
        )
        LightDMGreeter.connect(
            'show-prompt',
            lambda greeter, msg, mtype: self._emit_signal(self.show_prompt, msg, mtype)
        )

    def _emit_signal(self, _signal, *args):
        self.property_changed.emit()
        QTimer().singleShot(300, lambda: _signal.emit(*args))

    @pyqtProperty(str, notify=property_changed)
    def authentication_user(self):
        return LightDMGreeter.get_authentication_user() or ''

    @pyqtProperty(bool, notify=noop_signal)
    def autologin_guest(self):
        return LightDMGreeter.get_autologin_guest_hint()

    @pyqtProperty(int, notify=noop_signal)
    def autologin_timeout(self):
        return LightDMGreeter.get_autologin_timeout_hint()

    @pyqtProperty(str, notify=noop_signal)
    def autologin_user(self):
        return LightDMGreeter.get_autologin_user_hint()

    @pyqtProperty(bool, notify=noop_signal)
    def can_hibernate(self):
        return LightDM.get_can_hibernate()

    @pyqtProperty(bool, notify=noop_signal)
    def can_restart(self):
        return LightDM.get_can_restart()

    @pyqtProperty(bool, notify=noop_signal)
    def can_shutdown(self):
        return LightDM.get_can_shutdown()

    @pyqtProperty(bool, notify=noop_signal)
    def can_suspend(self):
        return LightDM.get_can_suspend()

    @pyqtProperty(str, notify=noop_signal)
    def default_session(self):
        return LightDMGreeter.get_default_session_hint()

    @pyqtProperty(bool, notify=noop_signal)
    def has_guest_account(self):
        return LightDMGreeter.get_has_guest_account_hint()

    @pyqtProperty(bool, notify=noop_signal)
    def hide_users_hint(self):
        return LightDMGreeter.get_hide_users_hint()

    @pyqtProperty(str, notify=noop_signal)
    def hostname(self):
        return LightDM.get_hostname()

    @pyqtProperty(bool, notify=property_changed)
    def in_authentication(self):
        return LightDMGreeter.get_in_authentication()

    @pyqtProperty(bool, notify=property_changed)
    def is_authenticated(self):
        return LightDMGreeter.get_is_authenticated()

    @pyqtProperty(QVariant, notify=property_changed)
    def language(self):
        return language_to_dict(LightDM.get_language())

    @pyqtProperty(QVariant, notify=noop_signal)
    def languages(self):
        return [language_to_dict(lang) for lang in LightDM.get_languages()]

    @pyqtProperty(QVariant, notify=noop_signal)
    def layout(self):
        return layout_to_dict(LightDM.get_layout())

    @pyqtProperty(QVariant, notify=noop_signal)
    def layouts(self):
        return [layout_to_dict(layout) for layout in LightDM.get_layouts()]

    @pyqtProperty(bool, notify=noop_signal)
    def lock_hint(self):
        return LightDMGreeter.get_lock_hint()

    @pyqtProperty(QVariant, notify=property_changed)
    def remote_sessions(self):
        return [session_to_dict(session) for session in LightDM.get_remote_sessions()]

    @pyqtProperty(bool, notify=noop_signal)
    def select_guest_hint(self):
        return LightDMGreeter.get_select_guest_hint()

    @pyqtProperty(str, notify=noop_signal)
    def select_user_hint(self):
        return LightDMGreeter.get_select_user_hint() or ''

    @pyqtProperty(QVariant, notify=noop_signal)
    def sessions(self):
        return [session_to_dict(session) for session in LightDM.get_sessions()]

    @pyqtProperty(str, notify=noop_signal)
    def shared_data_directory(self):
        return self._shared_data_directory

    @pyqtProperty(bool, notify=noop_signal)
    def show_manual_login_hint(self):
        return LightDMGreeter.get_show_manual_login_hint()

    @pyqtProperty(bool, notify=noop_signal)
    def show_remote_login_hint(self):
        return LightDMGreeter.get_show_remote_login_hint()

    @pyqtProperty(str, notify=noop_signal)
    def themes_directory(self):
        return self._themes_directory

    @pyqtProperty(QVariant, notify=noop_signal)
    def users(self):
        return [user_to_dict(user) for user in LightDMUsers.get_users()]

    @pyqtSlot(str)
    def authenticate(self, username):
        LightDMGreeter.authenticate(username)
        self.property_changed.emit()

    @pyqtSlot()
    def authenticate_as_guest(self):
        LightDMGreeter.authenticate_as_guest()
        self.property_changed.emit()

    @pyqtSlot()
    def cancel_authentication(self):
        LightDMGreeter.cancel_authentication()
        self.property_changed.emit()

    @pyqtSlot()
    def cancel_autologin(self):
        LightDMGreeter.cancel_autologin()
        self.property_changed.emit()

    @pyqtSlot(result=bool)
    def hibernate(self):
        return LightDMGreeter.hibernate()

    @pyqtSlot(str)
    def respond(self, response):
        LightDMGreeter.respond(response)
        self.property_changed.emit()

    @pyqtSlot(result=bool)
    def restart(self):
        return LightDMGreeter.restart()

    @pyqtSlot(str)
    def set_language(self, lang):
        if self.is_authenticated:
            LightDMGreeter.set_language(lang)
            self.property_changed.emit()

    @pyqtSlot(result=bool)
    def shutdown(self):
        return LightDMGreeter.shutdown()

    @pyqtSlot(str, result=bool)
    def start_session(self, session):
        return LightDMGreeter.start_session_sync(session)

    @pyqtSlot(result=bool)
    def suspend(self):
        return LightDMGreeter.suspend()





