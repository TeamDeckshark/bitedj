#include "controllers/midi/portmidienumerator.h"

#include <portmidi.h>

#include <QRegularExpression>
#include <QSet>

#include "controllers/midi/portmidicontroller.h"
#include "moc_portmidienumerator.cpp"
#include "util/cmdlineargs.h"

namespace {

const auto kMidiThroughPortPrefix = QLatin1String("MIDI Through Port");

bool recognizeDevice(const PmDeviceInfo& deviceInfo) {
    // In developer mode we show the MIDI Through Port, otherwise ignore it
    // since it routinely causes trouble.
    return CmdlineArgs::Instance().getDeveloper() ||
            !QLatin1String(deviceInfo.name)
                     .startsWith(kMidiThroughPortPrefix, Qt::CaseInsensitive);
}

// Some platforms format MIDI device names as "deviceName MIDI ###" where
// ### is the instance # of the device. Therefore we want to link two
// devices that have an equivalent "deviceName" and ### section.
const QRegularExpression kMidiDeviceNameRegex(QStringLiteral("^(.*) MIDI (\\d+)( .*)?$"));

const QRegularExpression kInputRegex(QStringLiteral("^(.*) in( \\d+)?( .*)?$"),
        QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kOutputRegex(QStringLiteral("^(.*) out( \\d+)?( .*)?$"),
        QRegularExpression::CaseInsensitiveOption);

// This is a broad pattern that matches a text blob followed by a numeral
// potentially followed by non-numeric text. The non-numeric requirement is
// meant to avoid corner cases around devices with names like "Hercules RMX
// 2" where we would potentially confuse the number in the device name as
// the ordinal index of the device.
const QRegularExpression kDeviceNameRegex(QStringLiteral("^(.*) (\\d+)( [^0-9]+)?$"));

bool namesMatchRegexes(const QRegularExpression& kInputRegex,
        const QString& input_name,
        const QRegularExpression& kOutputRegex,
        const QString& output_name) {
    QRegularExpressionMatch inputMatch = kInputRegex.match(input_name);
    if (inputMatch.hasMatch()) {
        QString inputDeviceName = inputMatch.captured(1);
        QString inputDeviceIndex = inputMatch.captured(2);
        QRegularExpressionMatch outputMatch = kOutputRegex.match(output_name);
        if (outputMatch.hasMatch()) {
            QString outputDeviceName = outputMatch.captured(1);
            QString outputDeviceIndex = outputMatch.captured(2);
            if (outputDeviceName.compare(inputDeviceName, Qt::CaseInsensitive) == 0 &&
                outputDeviceIndex == inputDeviceIndex) {
                return true;
            }
        }
    }
    return false;
}

bool namesMatchMidiPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kMidiDeviceNameRegex, input_name, kMidiDeviceNameRegex, output_name);
}

bool namesMatchInOutPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kInputRegex, input_name, kOutputRegex, output_name);
}

bool namesMatchPattern(const QString& input_name,
        const QString& output_name) {
    return namesMatchRegexes(kDeviceNameRegex, input_name, kDeviceNameRegex, output_name);
}

bool namesMatchAllowableEdgeCases(const QString& input_name,
        const QString& output_name) {
    // Mac OS 10.12 & Korg Kaoss DJ 1.6:
    // Korg Kaoss DJ has input 'KAOSS DJ CONTROL' and output 'KAOSS DJ SOUND'.
    // This means it doesn't pass the shouldLinkInputToOutput test. Without an
    // output linked, the MIDI output for the device fails, as the device is
    // NULL in PortMidiController
    if (input_name == "KAOSS DJ CONTROL" && output_name == "KAOSS DJ SOUND") {
        return true;
    }
    // Ableton Push on Windows
    // Shows 2 different devices for MIDI input and output.
    if (input_name == "MIDIIN2 (Ableton Push)" && output_name == "MIDIOUT2 (Ableton Push)") {
        return true;
    }

    // Novation Launchpad X (macOS)
    if (input_name == "Launchpad X LPX DAW Out" && output_name == "Launchpad X LPX DAW In") {
        return true;
    }

    return false;
}

} // namespace

PortMidiEnumerator::PortMidiEnumerator() {
    // Bite DJ: defer Pm_Initialize to the first queryDevices() call.
    // ControllerManager is constructed on the GUI thread and then
    // moveToThread'd to its own thread; queryDevices() runs on that
    // controller thread. PortMIDI on Linux uses ALSA sequencer clients
    // that bind to the calling thread, so initializing here (GUI thread)
    // and tearing down or re-initializing in queryDevices (controller
    // thread) crosses thread affinity and can crash ALSA sequencer state
    // when a MIDI device is opened later. Doing Pm_Initialize on first
    // queryDevices keeps every PortMIDI call on the same thread.
}

PortMidiEnumerator::~PortMidiEnumerator() {
    qDebug() << "Deleting PortMIDI devices...";
    QListIterator<Controller*> dev_it(m_devices);
    while (dev_it.hasNext()) {
        delete dev_it.next();
    }
    PmError err = Pm_Terminate();
    // Based on reading the source, it's not possible for this to fail.
    if (err != pmNoError) {
        qWarning() << "PortMidi error:" << Pm_GetErrorText(err);
    }
}

bool shouldLinkInputToOutput(const QString& input_name,
        const QString& output_name) {
    // Early exit.
    if (input_name == output_name || namesMatchAllowableEdgeCases(input_name, output_name)) {
        return true;
    }

    // Some device drivers prepend "To" and "From" to the names of their MIDI
    // ports. If the output and input device names don't match, let's try
    // trimming those words from the start, and seeing if they then match.

    // Ignore "From" text in the beginning of device input name.
    QString input_name_stripped = input_name;
    if (input_name.indexOf("from", 0, Qt::CaseInsensitive) == 0) {
        input_name_stripped = input_name.right(input_name.length() - 4);
    }

    // Ignore "To" text in the beginning of device output name.
    QString output_name_stripped = output_name;
    if (output_name.indexOf("to", 0, Qt::CaseInsensitive) == 0) {
        output_name_stripped = output_name.right(output_name.length() - 2);
    }

    if (output_name_stripped != input_name_stripped) {
        // Ignore " input " text in the device names
        int offset = input_name_stripped.indexOf(" input ", 0,
                                                 Qt::CaseInsensitive);
        if (offset != -1) {
            input_name_stripped = input_name_stripped.replace(offset, 7, " ");
        }

        // Ignore " output " text in the device names
        offset = output_name_stripped.indexOf(" output ", 0,
                                              Qt::CaseInsensitive);
        if (offset != -1) {
            output_name_stripped = output_name_stripped.replace(offset, 8, " ");
        }
    }

    if (input_name_stripped == output_name_stripped ||
        namesMatchMidiPattern(input_name_stripped, output_name_stripped) ||
        namesMatchMidiPattern(input_name, output_name) ||
        namesMatchInOutPattern(input_name_stripped, output_name_stripped) ||
        namesMatchInOutPattern(input_name, output_name) ||
        namesMatchPattern(input_name_stripped, output_name_stripped) ||
        namesMatchPattern(input_name, output_name)) {
        return true;
    }

    return false;
}

/** Enumerate the MIDI devices
  * This method needs a bit of intelligence because PortMidi (and the underlying MIDI APIs) like to split
  * output and input into separate devices. Eg. PortMidi would tell us the Hercules is two half-duplex devices.
  * To help simplify a lot of code, we're going to aggregate these two streams into a single full-duplex device.
  */
QList<Controller*> PortMidiEnumerator::queryDevices() {
    qDebug() << "Scanning PortMIDI devices:";

    // Tear down existing PortMidiController instances first. Their
    // destructors close any open Pm_OpenInput / Pm_OpenOutput handles, which
    // is required before Pm_Terminate is safe to invoke below.
    QListIterator<Controller*> dev_it(m_devices);
    while (dev_it.hasNext()) {
        delete dev_it.next();
    }
    m_devices.clear();

    // Bite DJ: PortMIDI snapshots the system device list at Pm_Initialize
    // and Pm_CountDevices keeps returning that cached count regardless of
    // USB hot-plug events. Cycle Pm_Terminate + Pm_Initialize so a Rescan
    // tap (which routes here via ControllerManager::setUpDevices →
    // updateControllerList) actually picks up devices plugged in after
    // startup. On the first invocation Pm_Terminate is a no-op (PortMIDI's
    // pm_initialized starts false now that we defer init to here).
    if (PmError termErr = Pm_Terminate(); termErr != pmNoError) {
        qWarning() << "PortMidi error during rescan terminate:"
                   << Pm_GetErrorText(termErr);
    }
    if (PmError initErr = Pm_Initialize(); initErr != pmNoError) {
        qWarning() << "PortMidi error during rescan initialize:"
                   << Pm_GetErrorText(initErr);
    }

    int iNumDevices = Pm_CountDevices();

    const PmDeviceInfo* inputDeviceInfo = nullptr;
    const PmDeviceInfo* outputDeviceInfo = nullptr;
    int inputDevIndex = -1;
    int outputDevIndex = -1;
    QMap<int, QString> unassignedOutputDevices;

    // Build a complete list of output devices for later pairing
    for (int i = 0; i < iNumDevices; i++) {
        const PmDeviceInfo* pDeviceInfo = Pm_GetDeviceInfo(i);
        VERIFY_OR_DEBUG_ASSERT(pDeviceInfo) {
            continue;
        }
        if (!recognizeDevice(*pDeviceInfo) || !pDeviceInfo->output) {
            continue;
        }
        qDebug() << " Found output device"
                 << "#" << i << pDeviceInfo->name;
        QString deviceName = pDeviceInfo->name;
        unassignedOutputDevices[i] = deviceName;
    }

    // Bite DJ: dedupe input devices by name. On Linux PortMIDI cycles
    // Pm_Terminate + Pm_Initialize between scans, but the ALSA sequencer
    // can leave the previous client lingering and re-register the same
    // ports under fresh indices on the new init. Without this, every
    // Rescan grows the consumer-visible controller list by one copy.
    // Identical names from genuinely separate hardware (rare in DJ rigs)
    // would also collapse to one row — acceptable given the alternative.
    QSet<QString> seenInputNames;

    // Search for input devices and pair them with output devices if applicable
    for (int i = 0; i < iNumDevices; i++) {
        const PmDeviceInfo* pDeviceInfo = Pm_GetDeviceInfo(i);
        VERIFY_OR_DEBUG_ASSERT(pDeviceInfo) {
            continue;
        }
        if (!recognizeDevice(*pDeviceInfo) || !pDeviceInfo->input) {
            // Is there a use case for output-only devices such as message
            // displays? Then this condition has to be split and
            // deviceInfo->output also needs to be checked and handled.
            continue;
        }

        const QString inputName(pDeviceInfo->name);
        if (seenInputNames.contains(inputName)) {
            qDebug() << " Skipping duplicate input device"
                     << "#" << i << pDeviceInfo->name;
            continue;
        }
        seenInputNames.insert(inputName);

        qDebug() << " Found input device"
                 << "#" << i << pDeviceInfo->name;
        inputDeviceInfo = pDeviceInfo;
        inputDevIndex = i;

        //Reset our output device variables before we look for one in case we find none.
        outputDeviceInfo = nullptr;
        outputDevIndex = -1;

        //Search for a corresponding output device
        QMapIterator<int, QString> j(unassignedOutputDevices);
        while (j.hasNext()) {
            j.next();

            QString deviceName = inputDeviceInfo->name;
            QString outputName = QString(j.value());

            if (shouldLinkInputToOutput(deviceName, outputName)) {
                outputDevIndex = j.key();
                outputDeviceInfo = Pm_GetDeviceInfo(outputDevIndex);

                unassignedOutputDevices.remove(outputDevIndex);

                qDebug() << "    Linking to output device #" << outputDevIndex << outputName;
                break;
            }
        }

        // So at this point, we either have an input-only MIDI device
        // (outputDeviceInfo == NULL) or we've found a matching output MIDI
        // device (outputDeviceInfo != NULL).

        //.... so create our (aggregate) MIDI device!
        PortMidiController* currentDevice =
                new PortMidiController(inputDeviceInfo,
                        outputDeviceInfo,
                        inputDevIndex,
                        outputDevIndex);
        m_devices.push_back(currentDevice);
    }
    return m_devices;
}
