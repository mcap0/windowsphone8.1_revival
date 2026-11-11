// ToolCall: one action of the assistant, as the command network fills it (Core/Command.cpp) and Actions runs it.
#pragma once

#include <string>

struct ToolCall {
    std::wstring name;
    Platform::String^ id;
    Windows::Data::Json::JsonObject^ args;
};
