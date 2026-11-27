/*
    Copyright 2016-2025 melonDS team
    Copyright 2026 melonDS Netplay contributors

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NETPLAYLOBBYDIALOG_H
#define NETPLAYLOBBYDIALOG_H

#include <QDialog>
#include <QMap>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTimer;
class QUdpSocket;
class QWidget;

struct DiscoveredRoom
{
    QString roomId;
    QString roomName;
    QString hostName;
    QString game;
    QString romHash;
    QString addr;
    int port = 7090;
    int players = 1;
    int maxPlayers = 4;
    bool hasPassword = false;
    bool isPrivate = false;
    bool hasLan = false;
    qint64 lastSeenMs = 0;
    qint64 lastLanMs = 0;
};

class NetplayLobbyDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NetplayLobbyDialog(QWidget* parent = nullptr, int initialTab = 0);
    ~NetplayLobbyDialog() override = default;

    static void openDlg(QWidget* parent, int initialTab = 0);

private slots:
    void onChooseAvatar();
    void onChooseBiosFolder();
    void onChooseRom();
    void onRefreshLobbies();
    void onLanBeaconReadyRead();
    void onLobbySelectionChanged();
    void onJoinSelectedLobby();
    void onHostStart();
    void onJoinDirect();

private:
    void refreshMissingWarning();
    void rebuildLobbyTable();
    bool validateSetup(QString& outRomPath);
    void joinDiscoveredRoom(const DiscoveredRoom& r, const QString& enteredPass);
    void launchNetplay(const QStringList& extraArgs, const QString& romPath);
    QString findDefaultRom() const;

    QTabWidget* tabs = nullptr;
    QLabel* lblAvatar = nullptr;
    QLineEdit* editPlayerName = nullptr;

    // Shown only when BIOS/Firmware or ROM are missing
    QWidget* missingBar = nullptr;
    QLabel* lblMissing = nullptr;
    QPushButton* btnFixBios = nullptr;
    QPushButton* btnFixRom = nullptr;

    QString currentRomPath;
    QLabel* lblHostRom = nullptr;

    // Tab 0: Join (Room ID / IP + Lobbies in one single tab)
    QString lobbyServerUrl;
    QLineEdit* editJoinTarget = nullptr;
    QLineEdit* editJoinPass = nullptr;
    QLineEdit* editLobbyFilter = nullptr;
    QTableWidget* tblLobbies = nullptr;
    QPushButton* btnRefresh = nullptr;

    // Tab 1: Host
    QLineEdit* editHostRoomId = nullptr;
    QLineEdit* editRoomName = nullptr;
    QLineEdit* editHostPass = nullptr;
    QSpinBox* spinMaxPlayers = nullptr;
    QCheckBox* chkPublicRoom = nullptr;

    QUdpSocket* lanSocket = nullptr;
    QNetworkAccessManager* netMgr = nullptr;
    QTimer* refreshTimer = nullptr;
    QMap<QString, DiscoveredRoom> rooms;
};

#endif // NETPLAYLOBBYDIALOG_H
