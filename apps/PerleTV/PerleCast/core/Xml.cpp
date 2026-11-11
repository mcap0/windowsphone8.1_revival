#include "Xml.h"
#include "Util.h"

#include <stdexcept>

namespace PerleCast {

namespace {
class Parser {
public:
    explicit Parser(const std::string& text) : text_(text), pos_(0) {}

    XmlNode ParseDocument()
    {
        SkipMisc();
        if (pos_ >= text_.size() || text_[pos_] != '<') throw std::runtime_error("No XML root element");
        XmlNode result = Element();
        return result;
    }

private:
    const std::string& text_;
    size_t pos_;

    bool Starts(const char* value) const { return text_.compare(pos_, std::char_traits<char>::length(value), value) == 0; }

    void SkipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_;
    }

    void SkipUntil(const char* end)
    {
        size_t found = text_.find(end, pos_);
        pos_ = found == std::string::npos ? text_.size() : found + std::char_traits<char>::length(end);
    }

    void SkipMisc()
    {
        while (pos_ < text_.size()) {
            SkipSpace();
            if (Starts("<?")) SkipUntil("?>");
            else if (Starts("<!--")) SkipUntil("-->");
            else if (Starts("<!DOCTYPE") || Starts("<!doctype")) SkipUntil(">");
            else break;
        }
    }

    std::string Name()
    {
        size_t start = pos_;
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>' || c == '=') break;
            ++pos_;
        }
        if (start == pos_) throw std::runtime_error("Malformed XML name");
        return text_.substr(start, pos_ - start);
    }

    std::string Quoted()
    {
        if (pos_ >= text_.size() || (text_[pos_] != '"' && text_[pos_] != '\'')) return "";
        char quote = text_[pos_++];
        size_t start = pos_;
        size_t end = text_.find(quote, pos_);
        if (end == std::string::npos) throw std::runtime_error("Unclosed XML attribute");
        pos_ = end + 1;
        return Util::XmlDecode(text_.substr(start, end - start));
    }

    XmlNode Element()
    {
        if (text_[pos_++] != '<') throw std::runtime_error("Expected XML element");
        if (pos_ < text_.size() && text_[pos_] == '/') throw std::runtime_error("Unexpected closing element");
        XmlNode node;
        node.name = Xml::LocalName(Name());
        while (pos_ < text_.size()) {
            SkipSpace();
            if (Starts("/>")) { pos_ += 2; return node; }
            if (Starts(">")) { ++pos_; break; }
            std::string key = Xml::LocalName(Name());
            SkipSpace();
            if (pos_ < text_.size() && text_[pos_] == '=') {
                ++pos_;
                SkipSpace();
                node.attributes[key] = Quoted();
            }
        }
        while (pos_ < text_.size()) {
            if (Starts("</")) {
                pos_ += 2;
                Name();
                SkipSpace();
                if (pos_ < text_.size() && text_[pos_] == '>') ++pos_;
                return node;
            }
            if (Starts("<!--")) { SkipUntil("-->"); continue; }
            if (Starts("<![CDATA[")) {
                pos_ += 9;
                size_t end = text_.find("]]>", pos_);
                if (end == std::string::npos) throw std::runtime_error("Unclosed CDATA");
                node.text.append(text_, pos_, end - pos_);
                pos_ = end + 3;
                continue;
            }
            if (Starts("<?")) { SkipUntil("?>"); continue; }
            if (text_[pos_] == '<') node.children.push_back(Element());
            else {
                size_t end = text_.find('<', pos_);
                if (end == std::string::npos) end = text_.size();
                node.text += Util::XmlDecode(text_.substr(pos_, end - pos_));
                pos_ = end;
            }
        }
        throw std::runtime_error("Unclosed XML element");
    }
};
}

std::string Xml::LocalName(const std::string& qualifiedName)
{
    size_t colon = qualifiedName.find(':');
    return colon == std::string::npos ? qualifiedName : qualifiedName.substr(colon + 1);
}

const XmlNode* XmlNode::Child(const std::string& localName) const
{
    for (size_t i = 0; i < children.size(); ++i)
        if (children[i].name == localName) return &children[i];
    return 0;
}

std::vector<const XmlNode*> XmlNode::Children(const std::string& localName) const
{
    std::vector<const XmlNode*> result;
    for (size_t i = 0; i < children.size(); ++i)
        if (children[i].name == localName) result.push_back(&children[i]);
    return result;
}

const XmlNode* XmlNode::Find(const std::string& localName) const
{
    if (name == localName) return this;
    for (size_t i = 0; i < children.size(); ++i) {
        const XmlNode* found = children[i].Find(localName);
        if (found) return found;
    }
    return 0;
}

std::string XmlNode::Text() const
{
    std::string result = text;
    for (size_t i = 0; i < children.size(); ++i) result += children[i].Text();
    return Util::Trim(result);
}

std::string XmlNode::Attribute(const std::string& localName) const
{
    std::map<std::string, std::string>::const_iterator it = attributes.find(localName);
    return it == attributes.end() ? "" : it->second;
}

XmlNode Xml::Parse(const std::string& document)
{
    std::string cleaned = document;
    if (cleaned.size() >= 3 && static_cast<unsigned char>(cleaned[0]) == 0xEF
        && static_cast<unsigned char>(cleaned[1]) == 0xBB && static_cast<unsigned char>(cleaned[2]) == 0xBF)
        cleaned.erase(0, 3);
    return Parser(cleaned).ParseDocument();
}

} // namespace PerleCast
