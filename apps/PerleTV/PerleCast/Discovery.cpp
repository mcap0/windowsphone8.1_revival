#include "Discovery.h"
#include "Net.h"
#include "WinRtText.h"
#include "core/Ssdp.h"

#include <collection.h>
#include <ppltasks.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.foundation.h>
#include <windows.networking.h>
#include <windows.networking.connectivity.h>
#include <windows.networking.sockets.h>
#include <windows.storage.streams.h>

using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage::Streams;

namespace PerleCast {

std::vector<DlnaDevice> Discovery::FindRenderers()
{
    std::vector<std::string> replies;
    std::mutex gate;

    DatagramSocket^ socket = ref new DatagramSocket();
    socket->MessageReceived += ref new Windows::Foundation::TypedEventHandler<
        DatagramSocket^, DatagramSocketMessageReceivedEventArgs^>(
        [&replies, &gate](DatagramSocket^,
                         DatagramSocketMessageReceivedEventArgs^ args)
        {
            try
            {
                DataReader^ reader = args->GetDataReader();
                unsigned int length = reader->UnconsumedBufferLength;
                Platform::String^ message = reader->ReadString(length);
                std::string value = ToUtf8(message);
                std::lock_guard<std::mutex> lock(gate);
                replies.push_back(value);
            }
            catch (Platform::Exception^) { }
        });

    try
    {
        concurrency::create_task(
            socket->BindServiceNameAsync(ref new Platform::String(L"0"))).get();

        HostName^ multicast = ref new HostName(
            ref new Platform::String(L"239.255.255.250"));
        std::string request = Ssdp::BuildSearchRequest(Ssdp::MediaRenderer, 3);

        for (int i = 0; i < 3; ++i)
        {
            try
            {
                IOutputStream^ output =
                    concurrency::create_task(socket->GetOutputStreamAsync(
                        multicast, ref new Platform::String(L"1900"))).get();
                DataWriter^ writer = ref new DataWriter(output);
                writer->WriteString(FromUtf8(request));
                concurrency::create_task(writer->StoreAsync()).get();
                concurrency::create_task(writer->FlushAsync()).get();
                writer->DetachStream();
                delete writer;
            }
            catch (Platform::Exception^) { }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    catch (Platform::Exception^) { }

    try { delete socket; }
    catch (Platform::Exception^) { }

    Net net;
    std::vector<DlnaDevice> devices;
    std::set<std::string> seen;

    for (size_t i = 0; i < replies.size(); ++i)
    {
        SsdpResponse response;
        if (!Ssdp::ParseResponse(replies[i], response) || response.location.empty())
            continue;
        if (!seen.insert(response.location).second)
            continue;

        HttpResult description = net.Get(response.location);
        if (description.code < 200 || description.code >= 300)
            continue;

        DlnaDevice device;
        if (!DeviceDescriptionParser::Parse(description.body, response.location, device))
            continue;
        if (device.avTransportControlUrl.empty())
            continue;
        devices.push_back(device);
    }

    return devices;
}

} // namespace PerleCast
