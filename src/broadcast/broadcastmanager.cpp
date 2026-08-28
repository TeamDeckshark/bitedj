#include "broadcast/broadcastmanager.h"

#include <shoutidjc/shout.h>

#include "broadcast/defs_broadcast.h"
#include "control/controlpushbutton.h"
#include "engine/sidechain/enginenetworkstream.h"
#include "moc_broadcastmanager.cpp"
#include "preferences/settingsmanager.h"
#include "soundio/soundmanager.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("BroadcastManager");
} // namespace

BroadcastManager::BroadcastManager(SettingsManager* pSettingsManager,
                                   SoundManager* pSoundManager)
        : m_pConfig(pSettingsManager->settings()),
          m_pBroadcastSettings(pSettingsManager->broadcastSettings()),
          m_pNetworkStream(pSoundManager->getNetworkStream()) {
    const bool persist = true;
    m_pBroadcastEnabled = new ControlPushButton(
            ConfigKey(BROADCAST_PREF_KEY,"enabled"), persist);
    m_pBroadcastEnabled->setButtonMode(ControlPushButton::TOGGLE);
    connect(m_pBroadcastEnabled,
            &ControlPushButton::valueChanged,
            this,
            &BroadcastManager::slotControlEnabled);

    m_pStatusCO = new ControlObject(ConfigKey(BROADCAST_PREF_KEY, "status"));
    m_pStatusCO->setReadOnly();
    m_pStatusCO->forceSet(STATUSCO_UNCONNECTED);

    // Initialize libshout
    shout_init();

    // Initialize connections list from the current state of BroadcastSettings.
    // Note this is normally a no-op: the destructor forces [Shoutcast],enabled
    // off, so broadcasting is off at every startup and connectionWanted() is
    // false for every profile until the user turns it on.
    addWantedConnections();

    // Connect add/remove profiles signals.
    // Passing the raw pointer from QSharedPointer to connect() is fine, since
    // connect is trusted that it won't delete the pointer
    connect(m_pBroadcastSettings.data(),
            &BroadcastSettings::profileAdded,
            this,
            &BroadcastManager::slotProfileAdded);
    connect(m_pBroadcastSettings.data(),
            &BroadcastSettings::profileRemoved,
            this,
            &BroadcastManager::slotProfileRemoved);
    connect(m_pBroadcastSettings.data(),
            &BroadcastSettings::profilesChanged,
            this,
            &BroadcastManager::slotProfilesChanged);
}

BroadcastManager::~BroadcastManager() {
    // Disable broadcast so when Mixxx starts again it will not connect.
    m_pBroadcastEnabled->set(0);

    delete m_pStatusCO;
    delete m_pBroadcastEnabled;

    shout_shutdown();
}

void BroadcastManager::setEnabled(bool value) {
    m_pBroadcastEnabled->set(value);
    // TODO(Palakis): apparently, calling set on a ControlObject and not through
    // a ControlProxy doesn't trigger the valueChanged signal here.
    // This is a quick fix, but there has to be a better way to do this, rather
    // than calling the associated slot directly.
    slotControlEnabled(value);
}

bool BroadcastManager::isEnabled() {
    return m_pBroadcastEnabled->toBool();
}

void BroadcastManager::slotControlEnabled(double v) {
    if (v > 1.0) {
        // Wrap around manually .
        // Wrapping around in WPushbutton does not work
        // since the status button has 4 states, but this CO is bool
        v = 0.0;
    }

    if (v > 0.0) {
        bool atLeastOneEnabled = false;
        QList<BroadcastProfilePtr> profiles = m_pBroadcastSettings->profiles();
        for (const BroadcastProfilePtr& profile : std::as_const(profiles)) {
            if (profile->getEnabled()) {
                atLeastOneEnabled = true;
                break;
            }
        }

        if (!atLeastOneEnabled) {
            m_pBroadcastEnabled->set(false);
            emit broadcastEnabled(0.0);
            QMessageBox::warning(nullptr, tr("Action failed"),
                                tr("Please enable at least one connection to use Live Broadcasting."));
            return;
        }

        slotProfilesChanged();
    } else {
        m_pBroadcastEnabled->set(false);
        m_pStatusCO->forceSet(STATUSCO_UNCONNECTED);
        const QList<BroadcastProfilePtr> profiles = m_pBroadcastSettings->profiles();
        for (BroadcastProfilePtr profile : profiles) {
            if (profile->connectionStatus() == BroadcastProfile::STATUS_FAILURE) {
                profile->setConnectionStatus(BroadcastProfile::STATUS_UNCONNECTED);
            }
        }
    }

    emit broadcastEnabled(v > 0.0);
}

void BroadcastManager::slotProfileAdded(BroadcastProfilePtr profile) {
    if (connectionWanted(profile)) {
        addConnection(profile);
    }
}

void BroadcastManager::slotProfileRemoved(BroadcastProfilePtr profile) {
    removeConnection(profile);
}

void BroadcastManager::slotProfilesChanged() {
    // Connections are created on demand rather than at startup, so a profile
    // that was just enabled (in the preferences, or by broadcasting being
    // switched on) has none yet. Create those first: the loop below only walks
    // connections that already exist. This is the single point every path that
    // can newly want a connection passes through.
    addWantedConnections();

    const QVector<NetworkOutputStreamWorkerPtr> workers = m_pNetworkStream->outputWorkers();
    for (const NetworkOutputStreamWorkerPtr& pWorker : workers) {
        ShoutConnectionPtr connection = qSharedPointerCast<ShoutConnection>(pWorker);
        if (connection) {
            BroadcastProfilePtr profile = connection->profile();
            if (profile->connectionStatus() == BroadcastProfile::STATUS_FAILURE
                    && !profile->getEnabled()) {
                profile->setConnectionStatus(BroadcastProfile::STATUS_UNCONNECTED);
            }
            connection->applySettings();
        }
    }
}

// A ShoutConnection is not a passive object. addConnection() registers it with
// EngineNetworkStream, and SoundDeviceNetwork::writeProcess() walks every
// registered worker from inside the audio callback -- so a connection that can
// never connect still costs work on the RT thread on every buffer, and its FIFO
// over/underflows are reported through SoundManager::underflowHappened().
//
// Upstream created one for every profile at startup and kept it for the whole
// session, including for profiles that were never enabled. On a device with no
// IP stack at all that meant a permanent phantom worker whose underruns were
// indistinguishable in the log from the sound card dropping out. Create one
// only while it could actually be used.
bool BroadcastManager::connectionWanted(const BroadcastProfilePtr& profile) const {
    return profile && profile->getEnabled() && m_pBroadcastEnabled->toBool();
}

void BroadcastManager::addWantedConnections() {
    const QList<BroadcastProfilePtr> profiles = m_pBroadcastSettings->profiles();
    for (const BroadcastProfilePtr& profile : profiles) {
        if (connectionWanted(profile)) {
            // Already-connected profiles are rejected by addConnection itself.
            addConnection(profile);
        }
    }
}

bool BroadcastManager::addConnection(BroadcastProfilePtr profile) {
    if (!profile) {
        return false;
    }

    if (findConnectionForProfile(profile).isNull() == false) {
        return false;
    }

    ShoutConnectionPtr connection(new ShoutConnection(profile, m_pConfig));
    m_pNetworkStream->addOutputWorker(connection);

    connect(profile.data(),
            &BroadcastProfile::connectionStatusChanged,
            this,
            &BroadcastManager::slotConnectionStatusChanged);

    kLogger.debug() << "addConnection: created connection for profile"
                    << profile->getProfileName();
    return true;
}

bool BroadcastManager::removeConnection(BroadcastProfilePtr profile) {
    if (!profile) {
        return false;
    }

    ShoutConnectionPtr connection = findConnectionForProfile(profile);
    if (connection) {
        disconnect(profile.data(),
                &BroadcastProfile::connectionStatusChanged,
                this,
                &BroadcastManager::slotConnectionStatusChanged);

        // Disabling the profile tells ShoutOutput's thread to disconnect
        connection->profile()->setEnabled(false);
        m_pNetworkStream->removeOutputWorker(connection);

        kLogger.debug() << "removeConnection: removed connection for profile"
                        << profile->getProfileName();
        return true;
    }

    return false;
}

ShoutConnectionPtr BroadcastManager::findConnectionForProfile(BroadcastProfilePtr profile) {
    const QVector<NetworkOutputStreamWorkerPtr> workers = m_pNetworkStream->outputWorkers();
    for (const NetworkOutputStreamWorkerPtr& pWorker : workers) {
        ShoutConnectionPtr connection = qSharedPointerCast<ShoutConnection>(pWorker);
        if (connection.isNull()) {
            continue;
        }

        if (connection->profile() == profile) {
            return connection;
        }
    }

    return ShoutConnectionPtr();
}

void BroadcastManager::slotConnectionStatusChanged(int newState) {
    Q_UNUSED(newState);
    int enabledCount = 0, connectingCount = 0,
        connectedCount = 0, failedCount = 0;

    // Collect status info
    const QList<BroadcastProfilePtr> profiles = m_pBroadcastSettings->profiles();
    for (BroadcastProfilePtr profile : profiles) {
        if (!profile->getEnabled()) {
            continue;
        }
        enabledCount++;

        int status = profile->connectionStatus();
        if (status == BroadcastProfile::STATUS_FAILURE) {
            failedCount++;
        }
        else if (status == BroadcastProfile::STATUS_CONNECTING) {
            connectingCount++;
        }
        else if (status == BroadcastProfile::STATUS_CONNECTED) {
            connectedCount++;
        }
    }

    // Changed global status indicator depending on global connections status
    if (enabledCount < 1) {
        // Disable Live Broadcasting if all connections are disabled manually.
        // Calling setEnabled will also update the status CO to UNCONNECTED
        setEnabled(false);
    }
    else if (failedCount >= enabledCount) {
        m_pStatusCO->forceSet(STATUSCO_FAILURE);
    }
    else if (failedCount > 0 && failedCount < enabledCount) {
        m_pStatusCO->forceSet(STATUSCO_WARNING);
    }
    else if (connectingCount > 0) {
        m_pStatusCO->forceSet(STATUSCO_CONNECTING);
    }
    else if (connectedCount > 0) {
        m_pStatusCO->forceSet(STATUSCO_CONNECTED);
    }
    else {
        m_pStatusCO->forceSet(STATUSCO_UNCONNECTED);
    }
}
