// Build:
// g++ -std=c++11 -O1 -I<tdlib>/td/telegram/.. -I<tdbuild>/... tdclient.cpp -L<tdbuild> -ltdjson -Wl,-rpath,<tdbuild> -o tdclient

#include <td/telegram/td_json_client.h>

#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

static void skip_whitespace(const char *&position) {
  while (*position == ' ' || *position == '\t' || *position == '\r' ||
         *position == '\n') {
    ++position;
  }
}

static void append_utf8(std::string &output, unsigned int codepoint) {
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

static bool parse_hex_quad(const char *&position, unsigned int &value) {
  value = 0;
  for (int i = 0; i < 4; ++i) {
    const char ch = *position++;
    value <<= 4;
    if (ch >= '0' && ch <= '9') {
      value |= static_cast<unsigned int>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value |= static_cast<unsigned int>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      value |= static_cast<unsigned int>(ch - 'A' + 10);
    } else {
      return false;
    }
  }
  return true;
}

static bool parse_json_string(const char *&position, std::string &output) {
  if (*position != '"') {
    return false;
  }
  ++position;

  while (*position != '\0') {
    const unsigned char ch = static_cast<unsigned char>(*position++);
    if (ch == '"') {
      return true;
    }
    if (ch < 0x20) {
      return false;
    }
    if (ch != '\\') {
      output.push_back(static_cast<char>(ch));
      continue;
    }

    const char escape = *position++;
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        output.push_back(escape);
        break;
      case 'b':
        output.push_back('\b');
        break;
      case 'f':
        output.push_back('\f');
        break;
      case 'n':
        output.push_back('\n');
        break;
      case 'r':
        output.push_back('\r');
        break;
      case 't':
        output.push_back('\t');
        break;
      case 'u': {
        unsigned int codepoint;
        if (!parse_hex_quad(position, codepoint)) {
          return false;
        }
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          if (position[0] != '\\' || position[1] != 'u') {
            return false;
          }
          position += 2;
          unsigned int low_surrogate;
          if (!parse_hex_quad(position, low_surrogate) ||
              low_surrogate < 0xdc00 || low_surrogate > 0xdfff) {
            return false;
          }
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                      (low_surrogate - 0xdc00);
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          return false;
        }
        append_utf8(output, codepoint);
        break;
      }
      default:
        return false;
    }
  }

  return false;
}

static bool skip_json_value(const char *&position, unsigned int depth);

static bool skip_json_object(const char *&position, unsigned int depth) {
  ++position;
  skip_whitespace(position);
  if (*position == '}') {
    ++position;
    return true;
  }

  while (*position != '\0') {
    std::string key;
    if (!parse_json_string(position, key)) {
      return false;
    }
    skip_whitespace(position);
    if (*position != ':') {
      return false;
    }
    ++position;
    skip_whitespace(position);
    if (!skip_json_value(position, depth + 1)) {
      return false;
    }
    skip_whitespace(position);
    if (*position == '}') {
      ++position;
      return true;
    }
    if (*position != ',') {
      return false;
    }
    ++position;
    skip_whitespace(position);
  }

  return false;
}

static bool skip_json_array(const char *&position, unsigned int depth) {
  ++position;
  skip_whitespace(position);
  if (*position == ']') {
    ++position;
    return true;
  }

  while (*position != '\0') {
    if (!skip_json_value(position, depth + 1)) {
      return false;
    }
    skip_whitespace(position);
    if (*position == ']') {
      ++position;
      return true;
    }
    if (*position != ',') {
      return false;
    }
    ++position;
    skip_whitespace(position);
  }

  return false;
}

static bool skip_json_value(const char *&position, unsigned int depth) {
  if (depth > 128 || *position == '\0') {
    return false;
  }
  if (*position == '"') {
    std::string ignored;
    return parse_json_string(position, ignored);
  }
  if (*position == '{') {
    return skip_json_object(position, depth);
  }
  if (*position == '[') {
    return skip_json_array(position, depth);
  }

  const char *start = position;
  while (*position != '\0' && *position != ',' && *position != ']' &&
         *position != '}' && *position != ' ' && *position != '\t' &&
         *position != '\r' && *position != '\n') {
    ++position;
  }
  return position != start;
}

static bool extract_root_type(const char *json, std::string &type) {
  if (json == NULL) {
    return false;
  }

  const char *position = json;
  skip_whitespace(position);
  if (*position != '{') {
    return false;
  }

  ++position;
  skip_whitespace(position);
  if (*position == '}') {
    return false;
  }

  while (*position != '\0') {
    std::string key;
    if (!parse_json_string(position, key)) {
      return false;
    }
    skip_whitespace(position);
    if (*position != ':') {
      return false;
    }
    ++position;
    skip_whitespace(position);

    if (key == "@type") {
      return parse_json_string(position, type);
    }
    if (!skip_json_value(position, 0)) {
      return false;
    }

    skip_whitespace(position);
    if (*position == '}') {
      return false;
    }
    if (*position != ',') {
      return false;
    }
    ++position;
    skip_whitespace(position);
  }

  return false;
}

static std::string json_quote(const char *value) {
  static const char hex[] = "0123456789abcdef";
  std::string result;
  result.push_back('"');

  for (const unsigned char *position =
           reinterpret_cast<const unsigned char *>(value);
       *position != '\0'; ++position) {
    const unsigned char ch = *position;
    switch (ch) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\b':
        result += "\\b";
        break;
      case '\f':
        result += "\\f";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (ch < 0x20) {
          result += "\\u00";
          result.push_back(hex[ch >> 4]);
          result.push_back(hex[ch & 0x0f]);
        } else {
          result.push_back(static_cast<char>(ch));
        }
        break;
    }
  }

  result.push_back('"');
  return result;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    return 2;
  }

  errno = 0;
  char *end = NULL;
  long api_id = std::strtol(argv[1], &end, 10);
  if (errno == ERANGE || end == argv[1] || *end != '\0' ||
      api_id < INT_MIN || api_id > INT_MAX) {
    return 2;
  }

  void *client = td_json_client_create();
  if (client == NULL) {
    return 1;
  }

  td_json_client_execute(
      client,
      "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":5}");

  std::string parameters =
      "{\"@type\":\"setTdlibParameters\",\"use_test_dc\":true,\"api_id\":" +
      std::to_string(api_id) + ",\"api_hash\":" + json_quote(argv[2]) +
      ",\"database_directory\":" + json_quote(argv[3]) +
      ",\"use_message_database\":false,\"use_secret_chats\":false,"
      "\"system_language_code\":\"en\",\"device_model\":\"pc\","
      "\"application_version\":\"1.0\",\"system_version\":\"linux\","
      "\"use_file_database\":false,\"use_chat_info_database\":false}";

  td_json_client_send(client, parameters.c_str());

  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);

  while (std::chrono::steady_clock::now() < deadline) {
    const char *response = td_json_client_receive(client, 10.0);
    std::string type;
    if (extract_root_type(response, type)) {
      std::fwrite(type.data(), 1, type.size(), stdout);
      std::fputc('\n', stdout);
      std::fflush(stdout);
    }
  }

  td_json_client_destroy(client);
  return 0;
}
