#ifndef PERLECAST_XML_H
#define PERLECAST_XML_H

#include <map>
#include <string>
#include <vector>

namespace PerleCast {

struct XmlNode {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::vector<XmlNode> children;
    std::string text;

    const XmlNode* Child(const std::string& localName) const;
    std::vector<const XmlNode*> Children(const std::string& localName) const;
    const XmlNode* Find(const std::string& localName) const;
    std::string Text() const;
    std::string Attribute(const std::string& localName) const;
};

class Xml {
public:
    static XmlNode Parse(const std::string& document);
    static std::string LocalName(const std::string& qualifiedName);
};

} // namespace PerleCast

#endif
