// Command: the network's intent and tagged words -> the function's arguments (the names Actions takes). Numbers, durations, times and days are plain code here;
// the network only says which word is which. Plain C++11, tested on the PC.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Text.h"
#include "Wsp.h"

namespace assist {

struct Command {
    std::string name;  // function name, or "ora"/"data" (answered here, see say)
    std::vector<std::pair<std::string, std::string> > args;  // UTF-8
    std::string say;   // ora/data: the answer
    std::string why;   // when BuildCommand fails: what was missing (for the log)
    std::string Arg(const std::string& key) const;
};

// targets: the names Actions::Open knows (UTF-8). false = something needed is missing: a Google search.
bool BuildCommand(const Wsp& net, const WspResult& r, const LocalTime& now, const std::vector<std::string>& targets,
                  Command& out);

// the network on the whole text, then (if that gives nothing sure) on each part of it: "domani ho il treno presto,
// svegliami alle cinque" -> the part that is a command. Parts: commas, and before "che"/"perché"/"ma".
bool Understand(const Wsp& net, const std::string& text, const LocalTime& now, const std::vector<std::string>& targets,
                Command& out, WspResult& used);

// what the phone cannot do goes to Google : the sentence without the openings and the
// politeness around it ("in pratica volevo sapere CHI HA VINTO LA PARTITA per favore"). Empty: nothing to search.
std::string SearchQuery(const Wsp& net, const std::string& text);

// "martedì 3 novembre"
std::string ItalianDate(int year, int month, int day);

}  // namespace assist
