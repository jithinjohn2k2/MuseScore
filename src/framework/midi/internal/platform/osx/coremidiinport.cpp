/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "coremidiinport.h"

#include <CoreAudio/HostTime.h>
#include <CoreServices/CoreServices.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>

#include "midierrors.h"
#include "log.h"

using namespace mu;
using namespace mu::midi;

struct mu::midi::CoreMidiInPort::Core {
    MIDIClientRef client = 0;
    MIDIPortRef inputPort = 0;
    MIDIEndpointRef sourceId = 0;
    int deviceID = -1;
};

CoreMidiInPort::CoreMidiInPort()
    : AbstractMidiInPort(), m_core(std::make_unique<Core>())
{
}

CoreMidiInPort::~CoreMidiInPort()
{
    if (isConnected()) {
        disconnect();
    }

    if (m_core->inputPort) {
        MIDIPortDispose(m_core->inputPort);
    }

    if (m_core->client) {
        MIDIClientDispose(m_core->client);
    }
}

void CoreMidiInPort::init()
{
    initCore();

    AbstractMidiInPort::init();
}

MidiDeviceList CoreMidiInPort::devices() const
{
    MidiDeviceList ret;

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
    ItemCount sources = MIDIGetNumberOfSources();
    for (ItemCount sourceIndex = 0; sourceIndex <= sources; sourceIndex++) {
        MIDIEndpointRef sourceRef = MIDIGetSource(sourceIndex);
        if (sourceRef != 0) {
            CFStringRef stringRef = 0;
            char name[256];

            if (MIDIObjectGetStringProperty(sourceRef, kMIDIPropertyDisplayName, &stringRef) != noErr) {
                LOGE() << "Can't get property kMIDIPropertyDisplayName";
                continue;
            }
            CFStringGetCString(stringRef, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(stringRef);

            MidiDevice dev;
            dev.id = std::to_string(sourceIndex);
            dev.name = name;

            ret.push_back(std::move(dev));
        }
    }

    return ret;
}

async::Notification CoreMidiInPort::devicesChanged() const
{
    return m_devicesChanged;
}

void CoreMidiInPort::initCore()
{
    OSStatus result;

    static auto onCoreMidiNotificationReceived = [](const MIDINotification* notification, void* refCon) {
        auto self = static_cast<CoreMidiInPort*>(refCon);
        IF_ASSERT_FAILED(self) {
            return;
        }

        switch (notification->messageID) {
        case kMIDIMsgObjectAdded:
        case kMIDIMsgObjectRemoved: {
            if (notification->messageSize != sizeof(MIDIObjectAddRemoveNotification)) {
                LOGW() << "Received corrupted MIDIObjectAddRemoveNotification";
                break;
            }

            auto addRemoveNotification = (const MIDIObjectAddRemoveNotification*)notification;

            if (addRemoveNotification->childType != kMIDIObjectType_Source) {
                break;
            }

            if (notification->messageID == kMIDIMsgObjectRemoved) {
                MIDIObjectRef removedObject = addRemoveNotification->child;

                if (self->isConnected() && removedObject == self->m_core->sourceId) {
                    self->disconnect();
                }
            }

            self->devicesChanged().notify();
        } break;

        case kMIDIMsgPropertyChanged: {
            if (notification->messageSize != sizeof(MIDIObjectPropertyChangeNotification)) {
                LOGW() << "Received corrupted MIDIObjectPropertyChangeNotification";
                break;
            }

            auto propertyChangeNotification = (const MIDIObjectPropertyChangeNotification*)notification;

            if (propertyChangeNotification->objectType != kMIDIObjectType_Device
                && propertyChangeNotification->objectType != kMIDIObjectType_Source) {
                break;
            }

            if (CFStringCompare(propertyChangeNotification->propertyName, kMIDIPropertyDisplayName, 0) == kCFCompareEqualTo
                || CFStringCompare(propertyChangeNotification->propertyName, kMIDIPropertyName, 0) == kCFCompareEqualTo) {
                self->devicesChanged().notify();
            }
        } break;

        // General message that should be ignored because we handle specific ones
        case kMIDIMsgSetupChanged:

        case kMIDIMsgThruConnectionsChanged:
        case kMIDIMsgSerialPortOwnerChanged:

        case kMIDIMsgIOError:
            break;
        }
    };

    QString name = "MuseScore";
    result = MIDIClientCreate(name.toCFString(), onCoreMidiNotificationReceived, this, &m_core->client);
    IF_ASSERT_FAILED(result == noErr) {
        LOGE() << "failed create midi input client";
        return;
    }

    QString portName = "MuseScore MIDI input port";
    if (__builtin_available(macOS 11.0, *)) {
        MIDIReceiveBlock receiveBlock = ^ (const MIDIEventList* eventList, void* /*srcConnRefCon*/) {
            const MIDIEventPacket* packet = eventList->packet;
            std::vector<std::pair<tick_t, Event> > events;

            for (UInt32 index = 0; index < eventList->numPackets; index++) {
                // Handle packet
                if (packet->wordCount != 0 && packet->wordCount <= 4) {
                    Event e = Event::fromRawData(packet->words, packet->wordCount);
                    if (e) {
                        events.push_back({ (tick_t)packet->timeStamp, e });
                    }
                } else if (packet->wordCount > 4) {
                    LOGW() << "unsupported midi message size " << packet->wordCount << " bytes";
                }

                packet = MIDIEventPacketNext(packet);
            }

            doEventsRecived(events);
        };

        result
            = MIDIInputPortCreateWithProtocol(m_core->client, portName.toCFString(), kMIDIProtocol_2_0, &m_core->inputPort, receiveBlock);
    } else {
        MIDIReadBlock readBlock = ^ (const MIDIPacketList* packetList, void* /*srcConnRefCon*/)
        {
            const MIDIPacket* packet = packetList->packet;
            std::vector<std::pair<tick_t, Event> > events;

            for (UInt32 index = 0; index < packetList->numPackets; index++) {
                if (packet->length != 0 && packet->length <= 4) {
                    uint32_t message(0);
                    memcpy(&message, packet->data, std::min(sizeof(message), sizeof(char) * packet->length));

                    auto e = Event::fromMIDI10Package(message).toMIDI20();
                    if (e) {
                        events.push_back({ (tick_t)packet->timeStamp, e });
                    }
                } else if (packet->length > 4) {
                    LOGW() << "unsupported midi message size " << packet->length << " bytes";
                }

                packet = MIDIPacketNext(packet);
            }

            doEventsRecived(events);
        };

        result = MIDIInputPortCreateWithBlock(m_core->client, portName.toCFString(), &m_core->inputPort, readBlock);
    }

    IF_ASSERT_FAILED(result == noErr) {
        LOGE() << "failed create midi input port";
    }
}

Ret CoreMidiInPort::connect(const MidiDeviceID& deviceID)
{
    if (isConnected()) {
        disconnect();
    }

    if (!m_core->client) {
        return make_ret(Err::MidiFailedConnect, "failed create client");
    }

    if (!m_core->inputPort) {
        return make_ret(Err::MidiFailedConnect, "failed create port");
    }

    m_core->deviceID = std::stoi(deviceID);
    m_core->sourceId = MIDIGetSource(m_core->deviceID);
    if (m_core->sourceId == 0) {
        return make_ret(Err::MidiFailedConnect, "failed get source");
    }

    m_deviceID = deviceID;
    return run();
}

void CoreMidiInPort::disconnect()
{
    if (!isConnected()) {
        return;
    }

    stop();

    m_core->sourceId = 0;
    m_deviceID.clear();
}

bool CoreMidiInPort::isConnected() const
{
    return m_core->sourceId && !m_deviceID.empty();
}

MidiDeviceID CoreMidiInPort::deviceID() const
{
    return m_deviceID;
}

Ret CoreMidiInPort::run()
{
    if (!isConnected()) {
        return make_ret(Err::MidiNotConnected);
    }

    OSStatus result = MIDIPortConnectSource(m_core->inputPort, m_core->sourceId, nullptr /*connRefCon*/);
    if (result == noErr) {
        m_running = true;
        return Ret(true);
    }
    m_running = false;
    return make_ret(Err::MidiFailedConnect);
}

void CoreMidiInPort::stop()
{
    if (!isConnected()) {
        LOGE() << "midi port is not connected";
        return;
    }

    OSStatus result = MIDIPortDisconnectSource(m_core->inputPort, m_core->sourceId);
    switch (result) {
    case kMIDINoConnection:
        LOGI() << "wasn't started";
        break;
    case noErr: break;
    default:
        LOGE() << "can't disconnect midi port " << result;
    }
    m_running = false;
}
