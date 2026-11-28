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

#include "NetplayLobbyDialog.h"
#include "NetSync.h"
#include "Config.h"
#include "Window.h"
#include "EmuInstance.h"
#include "EmuThread.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>

static const char* kDefaultLobbyUrl = "http://92.5.184.40:8787";
static const char* kOldDefaultLobbyUrl = "https://melonds-lobby.crackobdi.workers.dev";
static const char* kPrivatePrefix = "__priv__:";

static QString randomRoomId4()
{
    return QString::number(QRandomGenerator::global()->bounded(1000, 10000));
}

static QString computeRomHashHex(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    quint64 h = 1469598103934665603ULL;
    quint64 total = 0;
    char buf[65536];
    while (true)
    {
        qint64 n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        total += (quint64)n;
        for (qint64 i = 0; i < n; ++i)
        {
            h ^= (quint8)buf[i];
            h *= 1099511628211ULL;
        }
    }
    return QString::number((qulonglong)(h ^ total), 16);
}

static QString netplayBaseDir()
{
    QString appDir = QCoreApplication::applicationDirPath();
#ifdef __APPLE__
    QDir d(appDir);
    if (d.cd("..") && d.cd("..") && d.dirName().endsWith(".app") && d.cd(".."))
        return d.absolutePath();
#elif defined(_WIN32)
    QDir d(appDir);
    if (d.dirName().compare("bin", Qt::CaseInsensitive) == 0 && d.cd(".."))
        return d.absolutePath();
#endif
    return appDir;
}

static QString resolveBiosFile(Config::Table& cfg, const char* key, const char* fallbackFile)
{
    QString val = cfg.GetQString(key);
    QString appDir = QCoreApplication::applicationDirPath();
    QString baseDir = netplayBaseDir();
    if (!val.isEmpty())
    {
        QFileInfo fi(val);
        if (fi.isRelative())
        {
            QFileInfo fiSub(baseDir + "/BIOS and ROM/" + val);
            if (fiSub.exists()) return fiSub.absoluteFilePath();
            QFileInfo fiSubOld(baseDir + "/BIOS_and_ROMs/" + val);
            if (fiSubOld.exists()) return fiSubOld.absoluteFilePath();
            QFileInfo fiBase(baseDir + QDir::separator() + val);
            if (fiBase.exists()) return fiBase.absoluteFilePath();
            fi.setFile(appDir + QDir::separator() + val);
        }
        if (fi.exists()) return fi.absoluteFilePath();
    }
    QStringList fallbacks = {
        baseDir + "/BIOS and ROM/" + QString::fromLatin1(fallbackFile),
        baseDir + "/BIOS_and_ROMs/" + QString::fromLatin1(fallbackFile),
        baseDir + QDir::separator() + QString::fromLatin1(fallbackFile),
        appDir + QDir::separator() + QString::fromLatin1(fallbackFile),
    };
    for (const QString& p : fallbacks)
        if (QFileInfo::exists(p)) return p;
    return QString();
}

static QPixmap makeDefaultAvatar(const QString&)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor("#9aa8bd"), 1.5, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(1, 1, 30, 30, 6, 6);
    return pm;
}

static QPixmap loadSavedAvatar(Config::Table& localCfg, const QString& fallbackName)
{
    QString b64 = localCfg.GetQString("Netplay.AvatarData");
    if (!b64.isEmpty())
    {
        QByteArray raw = QByteArray::fromBase64(b64.toLatin1());
        QImage img;
        if (img.loadFromData(raw, "PNG") && !img.isNull())
        {
            QPixmap pm(32, 32);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            path.addRoundedRect(0, 0, 32, 32, 6, 6);
            p.setClipPath(path);
            p.drawImage(QRect(0, 0, 32, 32), img.scaled(32, 32, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            return pm;
        }
    }
    return makeDefaultAvatar(fallbackName);
}

void NetplayLobbyDialog::openDlg(QWidget* parent, int initialTab)
{
    NetplayLobbyDialog dlg(parent, initialTab);
    dlg.exec();
}

NetplayLobbyDialog::NetplayLobbyDialog(QWidget* parent, int initialTab)
    : QDialog(parent)
{
    setWindowTitle("Multiplayer");
    setMinimumSize(560, 400);

    Config::Table globalCfg = Config::GetGlobalTable();
    Config::Table localCfg = Config::GetLocalTable(0);
    lobbyServerUrl = globalCfg.GetQString("Netplay.LobbyURL").trimmed();
    if (lobbyServerUrl.isEmpty() || lobbyServerUrl == QString::fromLatin1(kOldDefaultLobbyUrl))
    {
        lobbyServerUrl = QString::fromLatin1(kDefaultLobbyUrl);
        globalCfg.SetQString("Netplay.LobbyURL", lobbyServerUrl);
    }
    currentRomPath = findDefaultRom();

    auto* root = new QVBoxLayout(this);

    auto* nameRow = new QHBoxLayout();
    lblAvatar = new QLabel(this);
    lblAvatar->setFixedSize(32, 32);
    lblAvatar->setPixmap(loadSavedAvatar(localCfg, localCfg.GetQString("Firmware.Username")));
    auto* btnAvatar = new QPushButton("Avatar...", this);
    connect(btnAvatar, &QPushButton::clicked, this, &NetplayLobbyDialog::onChooseAvatar);
    nameRow->addWidget(lblAvatar);
    nameRow->addWidget(btnAvatar);
    nameRow->addSpacing(8);
    nameRow->addWidget(new QLabel("Nickname:", this));
    editPlayerName = new QLineEdit(localCfg.GetQString("Firmware.Username").trimmed().left(10), this);
    editPlayerName->setMaxLength(10);
    editPlayerName->setMaximumWidth(180);
    nameRow->addWidget(editPlayerName);
    nameRow->addStretch();
    root->addLayout(nameRow);

    missingBar = new QWidget(this);
    auto* missRow = new QHBoxLayout(missingBar);
    missRow->setContentsMargins(0, 0, 0, 0);
    lblMissing = new QLabel(missingBar);
    lblMissing->setStyleSheet("color: #c0392b;");
    btnFixBios = new QPushButton("BIOS Folder...", missingBar);
    btnFixRom = new QPushButton("Select ROM...", missingBar);
    connect(btnFixBios, &QPushButton::clicked, this, &NetplayLobbyDialog::onChooseBiosFolder);
    connect(btnFixRom, &QPushButton::clicked, this, &NetplayLobbyDialog::onChooseRom);
    missRow->addWidget(lblMissing, 1);
    missRow->addWidget(btnFixBios);
    missRow->addWidget(btnFixRom);
    root->addWidget(missingBar);

    gameBar = new QWidget(this);
    auto* gameRow = new QHBoxLayout(gameBar);
    gameRow->setContentsMargins(0, 0, 0, 0);
    gameRow->addWidget(new QLabel("Game:", gameBar));
    comboRom = new QComboBox(gameBar);
    comboRom->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* btnBrowseRom = new QPushButton("Browse...", gameBar);
    connect(comboRom, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &NetplayLobbyDialog::onRomComboChanged);
    connect(btnBrowseRom, &QPushButton::clicked, this, &NetplayLobbyDialog::onChooseRom);
    gameRow->addWidget(comboRom, 1);
    gameRow->addWidget(btnBrowseRom);
    root->addWidget(gameBar);
    populateRomList();

    tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    // Tab 0: Join (Room ID / IP + Live Lobbies in one single tab)
    {
        auto* tab = new QWidget(tabs);
        auto* v = new QVBoxLayout(tab);

        auto* joinRow = new QHBoxLayout();
        joinRow->addWidget(new QLabel("Room ID / IP:", tab));
        editJoinTarget = new QLineEdit(tab);
        editJoinTarget->setPlaceholderText("e.g. 4829 or IP");
        editJoinPass = new QLineEdit(tab);
        editJoinPass->setEchoMode(QLineEdit::Password);
        editJoinPass->setPlaceholderText("Password (optional)");
        editJoinPass->setMaximumWidth(150);
        auto* btnJoin = new QPushButton("Join", tab);
        btnJoin->setDefault(true);
        connect(btnJoin, &QPushButton::clicked, this, &NetplayLobbyDialog::onJoinDirect);
        connect(editJoinTarget, &QLineEdit::returnPressed, this, &NetplayLobbyDialog::onJoinDirect);
        joinRow->addWidget(editJoinTarget, 1);
        joinRow->addWidget(editJoinPass);
        joinRow->addWidget(btnJoin);
        v->addLayout(joinRow);

        auto* bar = new QHBoxLayout();
        editLobbyFilter = new QLineEdit(tab);
        editLobbyFilter->setPlaceholderText("Search lobbies...");
        connect(editLobbyFilter, &QLineEdit::textChanged, this, &NetplayLobbyDialog::rebuildLobbyTable);
        btnRefresh = new QPushButton("Refresh", tab);
        connect(btnRefresh, &QPushButton::clicked, this, &NetplayLobbyDialog::onRefreshLobbies);
        bar->addWidget(editLobbyFilter, 1);
        bar->addWidget(btnRefresh);
        v->addLayout(bar);

        tblLobbies = new QTableWidget(0, 5, tab);
        tblLobbies->setHorizontalHeaderLabels({"Host", "Room", "Game", "Players", "Access"});
        tblLobbies->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        tblLobbies->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        tblLobbies->verticalHeader()->setVisible(false);
        tblLobbies->setSelectionBehavior(QAbstractItemView::SelectRows);
        tblLobbies->setSelectionMode(QAbstractItemView::SingleSelection);
        tblLobbies->setEditTriggers(QAbstractItemView::NoEditTriggers);
        connect(tblLobbies, &QTableWidget::itemSelectionChanged, this, &NetplayLobbyDialog::onLobbySelectionChanged);
        connect(tblLobbies, &QTableWidget::cellDoubleClicked, this, &NetplayLobbyDialog::onJoinSelectedLobby);
        v->addWidget(tblLobbies, 1);

        tabs->addTab(tab, "Join");
    }

    // Tab 1: Host
    {
        auto* tab = new QWidget(tabs);
        auto* v = new QVBoxLayout(tab);
        auto* form = new QFormLayout();

        auto* idRow = new QHBoxLayout();
        editHostRoomId = new QLineEdit(randomRoomId4(), tab);
        editHostRoomId->setMaxLength(4);
        editHostRoomId->setMaximumWidth(110);
        editHostRoomId->setPlaceholderText("4 digits");
        editHostRoomId->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9]{0,4}"), editHostRoomId));
        auto* btnNewId = new QPushButton("Random", tab);
        connect(btnNewId, &QPushButton::clicked, this, [this]() {
            editHostRoomId->setText(randomRoomId4());
        });
        idRow->addWidget(editHostRoomId);
        idRow->addWidget(btnNewId);
        idRow->addStretch();
        form->addRow("Room ID (max 4):", idRow);

        editRoomName = new QLineEdit(globalCfg.GetQString("Netplay.RoomName"), tab);
        if (editRoomName->text().isEmpty()) editRoomName->setText("My Lobby");
        editRoomName->setMaxLength(32);
        form->addRow("Room Name:", editRoomName);

        editHostPass = new QLineEdit(tab);
        editHostPass->setEchoMode(QLineEdit::Password);
        editHostPass->setPlaceholderText("Optional");
        form->addRow("Password:", editHostPass);

        spinMaxPlayers = new QSpinBox(tab);
        spinMaxPlayers->setRange(2, 4);
        int mp = globalCfg.GetInt("Netplay.MaxPlayers");
        spinMaxPlayers->setValue((mp >= 2 && mp <= 4) ? mp : 4);
        form->addRow("Players:", spinMaxPlayers);

        comboMode = new QComboBox(tab);
        comboMode->addItem("DS Download Play (1 cartridge)", false);
        comboMode->addItem("Multi-Card Wireless Play (all players)", true);
        comboMode->setCurrentIndex(globalCfg.GetBool("Netplay.MultiCard") ? 1 : 0);
        form->addRow("Mode:", comboMode);

        chkPublicRoom = new QCheckBox("Public Lobby", tab);
        chkPublicRoom->setChecked(globalCfg.GetBool("Netplay.Public"));
        form->addRow("", chkPublicRoom);

        v->addLayout(form);
        v->addStretch();

        auto* btnHost = new QPushButton("Create Room", tab);
        connect(btnHost, &QPushButton::clicked, this, &NetplayLobbyDialog::onHostStart);
        v->addWidget(btnHost);

        tabs->addTab(tab, "Host");
    }

    if (initialTab >= 0 && initialTab < tabs->count())
        tabs->setCurrentIndex(initialTab);

    netMgr = new QNetworkAccessManager(this);
    lanSocket = new QUdpSocket(this);
    lanSocket->bind(QHostAddress::AnyIPv4, NetSync::kLanDiscoveryPort, QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint);
    connect(lanSocket, &QUdpSocket::readyRead, this, &NetplayLobbyDialog::onLanBeaconReadyRead);

    refreshTimer = new QTimer(this);
    connect(refreshTimer, &QTimer::timeout, this, &NetplayLobbyDialog::onRefreshLobbies);
    refreshTimer->start(30000);

    refreshMissingWarning();
    onRefreshLobbies();
}

QString NetplayLobbyDialog::findDefaultRom() const
{
    Config::Table cfg = Config::GetGlobalTable();
    QString lastRom = cfg.GetQString("Netplay.LastROM");
    if (!lastRom.isEmpty() && QFileInfo::exists(lastRom))
        return lastRom;
    Config::Array recent = cfg.GetArray("RecentROM");
    if (recent.Size() > 0)
    {
        QString p = QString::fromStdString(recent.GetString(0));
        if (QFileInfo::exists(p)) return p;
    }
    for (const char* sub : {"/BIOS and ROM", "/BIOS_and_ROMs"})
    {
        QDir romSubDir(netplayBaseDir() + sub);
        QStringList subFiles = romSubDir.exists() ? romSubDir.entryList({"*.nds"}, QDir::Files) : QStringList();
        if (!subFiles.isEmpty())
            return romSubDir.absoluteFilePath(subFiles.first());
    }
    QDir baseDir(netplayBaseDir());
    QStringList ndsFiles = baseDir.entryList({"*.nds"}, QDir::Files);
    if (!ndsFiles.isEmpty())
        return baseDir.absoluteFilePath(ndsFiles.first());
    QDir appDir(QCoreApplication::applicationDirPath());
    ndsFiles = appDir.entryList({"*.nds"}, QDir::Files);
    if (!ndsFiles.isEmpty())
        return appDir.absoluteFilePath(ndsFiles.first());
    return QString();
}

void NetplayLobbyDialog::onChooseAvatar()
{
    QString file = QFileDialog::getOpenFileName(this, "Select Profile Picture",
        QString(), "Images (*.png *.jpg *.jpeg *.bmp *.webp)");
    if (file.isEmpty()) return;

    QImage img(file);
    if (img.isNull())
    {
        QMessageBox::warning(this, "Multiplayer", "Could not load selected image.");
        return;
    }

    int side = std::min(img.width(), img.height());
    QImage cropped = img.copy((img.width() - side) / 2, (img.height() - side) / 2, side, side)
                        .scaled(32, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray pngBytes;
    QBuffer buf(&pngBytes);
    buf.open(QIODevice::WriteOnly);
    cropped.save(&buf, "PNG");

    Config::Table localCfg = Config::GetLocalTable(0);
    localCfg.SetQString("Netplay.AvatarData", QString::fromLatin1(pngBytes.toBase64()));
    Config::Save();
    if (lblAvatar)
        lblAvatar->setPixmap(loadSavedAvatar(localCfg, editPlayerName->text()));
}

void NetplayLobbyDialog::refreshMissingWarning()
{
    Config::Table cfg = Config::GetGlobalTable();
    bool hasBios = !resolveBiosFile(cfg, "DS.BIOS7Path", "bios7.bin").isEmpty() &&
                   !resolveBiosFile(cfg, "DS.BIOS9Path", "bios9.bin").isEmpty() &&
                   !resolveBiosFile(cfg, "DS.FirmwarePath", "firmware.bin").isEmpty();
    bool hasRom = !currentRomPath.isEmpty() && QFileInfo::exists(currentRomPath);

    if (hasBios && hasRom)
    {
        missingBar->hide();
        return;
    }

    QStringList missing;
    if (!hasBios) missing << "BIOS/Firmware";
    if (!hasRom) missing << "ROM (.nds)";
    lblMissing->setText("Missing: " + missing.join(", "));
    btnFixBios->setVisible(!hasBios);
    btnFixRom->setVisible(!hasRom);
    missingBar->show();
}

void NetplayLobbyDialog::onChooseBiosFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Select folder with bios7.bin, bios9.bin & firmware.bin",
        netplayBaseDir());
    if (dir.isEmpty()) return;

    QDir d(dir);
    QString b7 = d.absoluteFilePath("bios7.bin");
    QString b9 = d.absoluteFilePath("bios9.bin");
    QString fw = d.absoluteFilePath("firmware.bin");
    if (!QFileInfo::exists(b7) || !QFileInfo::exists(b9) || !QFileInfo::exists(fw))
    {
        QMessageBox::warning(this, "Multiplayer",
            "The selected folder must contain bios7.bin, bios9.bin, and firmware.bin.");
        return;
    }

    Config::Table cfg = Config::GetGlobalTable();
    cfg.SetQString("DS.BIOS7Path", b7);
    cfg.SetQString("DS.BIOS9Path", b9);
    cfg.SetQString("DS.FirmwarePath", fw);
    cfg.SetBool("Emu.ExternalBIOSEnable", true);
    Config::Save();
    refreshMissingWarning();
}

void NetplayLobbyDialog::populateRomList()
{
    if (!comboRom) return;
    comboRom->blockSignals(true);
    comboRom->clear();

    QStringList seen;
    auto addCandidate = [&](const QString& path) {
        if (path.isEmpty() || !QFileInfo::exists(path)) return;
        QString canon = QFileInfo(path).canonicalFilePath();
        if (canon.isEmpty()) canon = QFileInfo(path).absoluteFilePath();
        if (seen.contains(canon, Qt::CaseInsensitive)) return;
        seen << canon;
        comboRom->addItem(QFileInfo(canon).fileName(), canon);
    };

    if (!currentRomPath.isEmpty())
        addCandidate(currentRomPath);

    for (const char* sub : {"/BIOS and ROM", "/BIOS_and_ROMs"})
    {
        QDir romSubDir(netplayBaseDir() + sub);
        if (!romSubDir.exists()) continue;
        for (const QString& f : romSubDir.entryList({"*.nds"}, QDir::Files, QDir::Name))
            addCandidate(romSubDir.absoluteFilePath(f));
    }

    QDir baseDir(netplayBaseDir());
    for (const QString& f : baseDir.entryList({"*.nds"}, QDir::Files, QDir::Name))
        addCandidate(baseDir.absoluteFilePath(f));

    Config::Table cfg = Config::GetGlobalTable();
    Config::Array recent = cfg.GetArray("RecentROM");
    for (size_t i = 0; i < recent.Size(); ++i)
        addCandidate(QString::fromStdString(recent.GetString(i)));

    if (comboRom->count() == 0)
    {
        comboRom->addItem("No .nds ROM found — click Browse...", QString());
        currentRomPath.clear();
        if (gameBar) gameBar->hide();
    }
    else
    {
        int idx = -1;
        if (!currentRomPath.isEmpty())
        {
            QString canonCur = QFileInfo(currentRomPath).canonicalFilePath();
            for (int i = 0; i < comboRom->count(); ++i)
            {
                if (comboRom->itemData(i).toString().compare(canonCur, Qt::CaseInsensitive) == 0)
                {
                    idx = i;
                    break;
                }
            }
        }
        if (idx < 0) idx = 0;
        comboRom->setCurrentIndex(idx);
        currentRomPath = comboRom->itemData(idx).toString();
        if (gameBar) gameBar->setVisible(comboRom->count() > 1);
    }
    comboRom->blockSignals(false);
}

void NetplayLobbyDialog::selectOrAddRom(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
    QString canon = QFileInfo(path).canonicalFilePath();
    if (canon.isEmpty()) canon = QFileInfo(path).absoluteFilePath();

    currentRomPath = canon;
    Config::Table cfg = Config::GetGlobalTable();
    cfg.SetQString("Netplay.LastROM", canon);
    Config::Save();

    if (comboRom)
    {
        comboRom->blockSignals(true);
        if (comboRom->count() == 1 && comboRom->itemData(0).toString().isEmpty())
            comboRom->clear();
        int idx = -1;
        for (int i = 0; i < comboRom->count(); ++i)
        {
            if (comboRom->itemData(i).toString().compare(canon, Qt::CaseInsensitive) == 0)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            comboRom->addItem(QFileInfo(canon).fileName(), canon);
            idx = comboRom->count() - 1;
        }
        comboRom->setCurrentIndex(idx);
        comboRom->blockSignals(false);
        if (gameBar) gameBar->setVisible(comboRom->count() > 1);
    }
    refreshMissingWarning();
}

void NetplayLobbyDialog::onRomComboChanged(int index)
{
    if (!comboRom || index < 0) return;
    QString path = comboRom->itemData(index).toString();
    currentRomPath = path;
    if (!path.isEmpty())
    {
        Config::Table cfg = Config::GetGlobalTable();
        cfg.SetQString("Netplay.LastROM", path);
        Config::Save();
    }
    refreshMissingWarning();
}

void NetplayLobbyDialog::onChooseRom()
{
    QString startDir = !currentRomPath.isEmpty()
        ? QFileInfo(currentRomPath).absolutePath()
        : netplayBaseDir();
    QString file = QFileDialog::getOpenFileName(this, "Select DS ROM", startDir, "Nintendo DS ROM (*.nds)");
    if (!file.isEmpty())
        selectOrAddRom(file);
}

static void parseRoomPrivacy(QString& roomName, bool& outIsPrivate)
{
    QString priv = QString::fromLatin1(kPrivatePrefix);
    if (roomName.startsWith(priv))
    {
        outIsPrivate = true;
        roomName = roomName.mid(priv.size());
    }
    else
    {
        outIsPrivate = false;
    }
}

void NetplayLobbyDialog::onLanBeaconReadyRead()
{
    while (lanSocket && lanSocket->hasPendingDatagrams())
    {
        QByteArray data;
        QHostAddress sender;
        data.resize((int)lanSocket->pendingDatagramSize());
        lanSocket->readDatagram(data.data(), data.size(), &sender);

        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) continue;
        QJsonObject obj = doc.object();
        if (obj.value("proto").toString() != "melonDS-Netplay") continue;

        QString roomId = obj.value("roomId").toString().toUpper();
        if (roomId.isEmpty()) continue;

        int players = obj.value("players").toInt(1);
        if (players <= 0) { rooms.remove(roomId); continue; }

        qint64 now = QDateTime::currentMSecsSinceEpoch();
        DiscoveredRoom r;
        r.roomId = roomId;
        r.roomName = obj.value("roomName").toString();
        parseRoomPrivacy(r.roomName, r.isPrivate);
        r.hostName = obj.value("hostName").toString("Host");
        r.game = obj.value("game").toString();
        r.romHash = obj.value("romHash").toString();
        r.addr = sender.toString();
        r.port = obj.value("localPort").toInt(7090);
        r.players = players;
        r.maxPlayers = obj.value("maxPlayers").toInt(4);
        r.hasPassword = obj.value("hasPassword").toBool(false);
        r.hasLan = true;
        r.lastSeenMs = now;
        r.lastLanMs = now;
        rooms[roomId] = r;
    }
    rebuildLobbyTable();
}

void NetplayLobbyDialog::onRefreshLobbies()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = rooms.begin(); it != rooms.end();)
    {
        if (now - it->lastSeenMs > 10000) it = rooms.erase(it);
        else ++it;
    }
    rebuildLobbyTable();

    QString base = lobbyServerUrl;
    while (base.endsWith('/')) base.chop(1);
    if (base.isEmpty()) return;

    QNetworkRequest req(QUrl(base + "/rooms"));
    QNetworkReply* reply = netMgr->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray arr = doc.isArray() ? doc.array() : doc.object().value("rooms").toArray();
        qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (const QJsonValue& val : arr)
        {
            QJsonObject obj = val.toObject();
            QString roomId = obj.value("roomId").toString().toUpper();
            int players = obj.value("players").toInt(1);
            if (roomId.isEmpty() || players <= 0) continue;

            // Keep LAN IP if this room was seen directly on local subnet to avoid hairpin-NAT failure
            if (rooms.contains(roomId) && rooms[roomId].hasLan && (nowMs - rooms[roomId].lastLanMs) < 10000)
            {
                rooms[roomId].players = players;
                rooms[roomId].lastSeenMs = nowMs;
                continue;
            }

            DiscoveredRoom r;
            r.roomId = roomId;
            r.roomName = obj.value("roomName").toString();
            parseRoomPrivacy(r.roomName, r.isPrivate);
            r.hostName = obj.value("hostName").toString("Host");
            r.game = obj.value("game").toString();
            r.romHash = obj.value("romHash").toString();
            r.addr = obj.value("addr").toString();
            r.port = obj.value("port").toInt(7090);
            r.players = players;
            r.maxPlayers = obj.value("maxPlayers").toInt(4);
            r.hasPassword = obj.value("hasPassword").toBool(false);
            r.hasLan = false;
            r.lastSeenMs = nowMs;
            rooms[roomId] = r;
        }
        rebuildLobbyTable();
    });
}

void NetplayLobbyDialog::rebuildLobbyTable()
{
    QString filter = editLobbyFilter->text().trimmed().toLower();
    tblLobbies->setRowCount(0);
    for (const auto& r : rooms)
    {
        if (r.isPrivate) continue;
        if (!filter.isEmpty())
        {
            QString hay = (r.hostName + " " + r.roomName + " " + r.game + " " + r.roomId).toLower();
            if (!hay.contains(filter)) continue;
        }
        int row = tblLobbies->rowCount();
        tblLobbies->insertRow(row);
        auto* hostItem = new QTableWidgetItem(r.hostName);
        hostItem->setData(Qt::UserRole, r.roomId);
        tblLobbies->setItem(row, 0, hostItem);
        tblLobbies->setItem(row, 1, new QTableWidgetItem(r.roomName));
        tblLobbies->setItem(row, 2, new QTableWidgetItem(r.game));
        tblLobbies->setItem(row, 3, new QTableWidgetItem(QString("%1/%2").arg(r.players).arg(r.maxPlayers)));
        tblLobbies->setItem(row, 4, new QTableWidgetItem(r.hasPassword ? "Password" : "Open"));
    }
}

void NetplayLobbyDialog::onLobbySelectionChanged()
{
    int row = tblLobbies->currentRow();
    if (row < 0) return;
    QString roomId = tblLobbies->item(row, 0)->data(Qt::UserRole).toString();
    if (!roomId.isEmpty())
        editJoinTarget->setText(roomId);
}

bool NetplayLobbyDialog::validateSetup(QString& outRomPath)
{
    QString name = editPlayerName->text().trimmed().left(10);
    if (name.isEmpty())
    {
        QMessageBox::warning(this, "Multiplayer", "Please enter a nickname.");
        editPlayerName->setFocus();
        return false;
    }

    Config::Table cfg = Config::GetGlobalTable();
    Config::Table localCfg = Config::GetLocalTable(0);
    localCfg.SetQString("Firmware.Username", name);

    if (resolveBiosFile(cfg, "DS.BIOS7Path", "bios7.bin").isEmpty() ||
        resolveBiosFile(cfg, "DS.BIOS9Path", "bios9.bin").isEmpty() ||
        resolveBiosFile(cfg, "DS.FirmwarePath", "firmware.bin").isEmpty())
    {
        refreshMissingWarning();
        QMessageBox::warning(this, "Multiplayer", "Missing bios7.bin, bios9.bin, or firmware.bin.");
        return false;
    }

    outRomPath = currentRomPath;
    if (outRomPath.isEmpty() || !QFileInfo::exists(outRomPath))
    {
        refreshMissingWarning();
        QMessageBox::warning(this, "Multiplayer", "Please select a valid .nds ROM file.");
        return false;
    }

    cfg.SetQString("Netplay.LastROM", outRomPath);
    cfg.SetQString("Netplay.RoomName", editRoomName->text().trimmed());
    cfg.SetInt("Netplay.MaxPlayers", spinMaxPlayers->value());
    if (comboMode) cfg.SetBool("Netplay.MultiCard", comboMode->currentData().toBool());
    cfg.SetBool("Netplay.Public", chkPublicRoom->isChecked());
    Config::Save();
    return true;
}

void NetplayLobbyDialog::launchNetplay(const QStringList& extraArgs, const QString& romPath)
{
    if (refreshTimer) refreshTimer->stop();

    QStringList args = extraArgs;
    args << "--netsync-name" << editPlayerName->text().trimmed().left(10);
    if (!lobbyServerUrl.isEmpty()) args << "--netsync-lobby" << lobbyServerUrl;
    args << romPath;

    if (auto* mw = qobject_cast<MainWindow*>(parentWidget()))
    {
        if (mw->getEmuInstance() && mw->getEmuInstance()->getEmuThread())
        {
            mw->getEmuInstance()->getEmuThread()->emuExit();
            mw->getEmuInstance()->getEmuThread()->wait();
        }
        mw->hide();
    }
    hide();
    QApplication::processEvents();

    bool launched = false;
#ifdef __APPLE__
    QString appBundle = QDir(QCoreApplication::applicationDirPath() + "/../..").canonicalPath();
    if (appBundle.endsWith(".app"))
    {
        QStringList openArgs;
        openArgs << "-n" << appBundle << "--args" << args;
        launched = QProcess::startDetached("/usr/bin/open", openArgs);
    }
    else
        launched = QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
#else
    launched = QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
#endif
    if (!launched)
    {
        if (auto* mw = qobject_cast<MainWindow*>(parentWidget())) mw->show();
        show();
        QMessageBox::critical(this, "Multiplayer", "Failed to launch multiplayer session.");
        return;
    }
    accept();
    QCoreApplication::quit();
}

void NetplayLobbyDialog::joinDiscoveredRoom(const DiscoveredRoom& r, const QString& enteredPass)
{
    QString romPath;
    if (!validateSetup(romPath)) return;

    if (!r.romHash.isEmpty())
    {
        QString targetHash = r.romHash.trimmed().toLower();
        QString curHash = computeRomHashHex(romPath).toLower();
        if (curHash != targetHash)
        {
            QString matchedPath;
            if (comboRom)
            {
                for (int i = 0; i < comboRom->count(); ++i)
                {
                    QString cand = comboRom->itemData(i).toString();
                    if (cand.isEmpty() || cand == romPath) continue;
                    if (computeRomHashHex(cand).toLower() == targetHash)
                    {
                        matchedPath = cand;
                        break;
                    }
                }
            }
            if (!matchedPath.isEmpty())
            {
                selectOrAddRom(matchedPath);
                romPath = matchedPath;
            }
            else
            {
                QString roomGame = r.game.isEmpty() ? "another game" : r.game;
                QString curGame = QFileInfo(romPath).fileName();
                QMessageBox::warning(this, "Wrong Game Selected",
                    QString("This room is playing \"%1\".\nYour selected game is \"%2\".\n\nPlease select the matching .nds ROM in the Game dropdown.")
                        .arg(roomGame, curGame));
                return;
            }
        }
    }

    QString pass = enteredPass;
    if (r.hasPassword && pass.isEmpty())
    {
        bool ok = false;
        pass = QInputDialog::getText(this, "Password",
            QString("Password for \"%1\":").arg(r.roomName), QLineEdit::Password, QString(), &ok);
        if (!ok) return;
    }

    QStringList args;
    args << "--netsync" << "client"
         << "--netsync-addr" << r.addr
         << "--netsync-port" << QString::number(r.port)
         << "--netsync-roomid" << r.roomId;
    if (!pass.isEmpty()) args << "--netsync-pass" << pass;
    launchNetplay(args, romPath);
}

void NetplayLobbyDialog::onJoinSelectedLobby()
{
    int row = tblLobbies->currentRow();
    if (row < 0) return;
    QString roomId = tblLobbies->item(row, 0)->data(Qt::UserRole).toString();
    if (!rooms.contains(roomId)) return;
    joinDiscoveredRoom(rooms[roomId], editJoinPass->text());
}

void NetplayLobbyDialog::onHostStart()
{
    QString romPath;
    if (!validateSetup(romPath)) return;

    QString roomId = editHostRoomId->text().trimmed().toUpper().left(4);
    if (roomId.isEmpty()) roomId = randomRoomId4();

    QString roomName = editRoomName->text().trimmed();
    if (roomName.isEmpty()) roomName = "My Lobby";

    QStringList args;
    args << "--netsync" << "host"
         << "--netsync-players" << QString("auto:%1").arg(spinMaxPlayers->value())
         << "--netsync-port" << "7090"
         << "--netsync-delay" << "0"
         << "--netsync-roomid" << roomId
         << "--netsync-room" << roomName;
    if (!editHostPass->text().isEmpty())
        args << "--netsync-pass" << editHostPass->text();
    if (chkPublicRoom->isChecked())
        args << "--netsync-public";
    if (comboMode && comboMode->currentData().toBool())
        args << "--netsync-multicard";

    launchNetplay(args, romPath);
}

void NetplayLobbyDialog::onJoinDirect()
{
    QString target = editJoinTarget->text().trimmed();
    QString upperTarget = target.toUpper();
    if (rooms.contains(upperTarget))
    {
        joinDiscoveredRoom(rooms[upperTarget], editJoinPass->text());
        return;
    }

    QString romPath;
    if (!validateSetup(romPath)) return;

    if (target.isEmpty())
    {
        QMessageBox::warning(this, "Multiplayer", "Please enter a Room ID or IP address.");
        editJoinTarget->setFocus();
        return;
    }

    QStringList args;
    args << "--netsync" << "client"
         << "--netsync-addr" << target
         << "--netsync-port" << "7090";
    if (!editJoinPass->text().isEmpty())
        args << "--netsync-pass" << editJoinPass->text();
    launchNetplay(args, romPath);
}
