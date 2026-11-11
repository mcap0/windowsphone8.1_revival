// Build:
// g++ -std=c++11 -O1 -I<tdlib>/td/telegram/.. -I<tdbuild>/... td_login.cpp -L<tdbuild> -ltdjson -Wl,-rpath,<tdbuild> -o td_login

#include <td/telegram/td_json_client.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

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
    if (*position == '\0') {
      return false;
    }
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

    if (*position == '\0') {
      return false;
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

static const char *find_member_value(const char *json, const char *name) {
  const char *position = json;
  while (*position != '\0') {
    if (*position != '"') {
      ++position;
      continue;
    }

    std::string key;
    if (!parse_json_string(position, key)) {
      return NULL;
    }
    skip_whitespace(position);
    if (key == name && *position == ':') {
      ++position;
      skip_whitespace(position);
      return position;
    }
  }
  return NULL;
}

static bool find_string_member(const char *json, const char *name,
                               std::string &value) {
  const char *position = find_member_value(json, name);
  return position != NULL && parse_json_string(position, value);
}

static bool read_api_id(const char *json, int &api_id) {
  const char *position = find_member_value(json, "api_id");
  if (position == NULL) {
    return false;
  }

  std::string quoted;
  if (*position == '"') {
    if (!parse_json_string(position, quoted)) {
      return false;
    }
    position = quoted.c_str();
  }

  errno = 0;
  char *end = NULL;
  const long value = std::strtol(position, &end, 10);
  if (errno == ERANGE || end == position || value <= 0 ||
      value > INT_MAX) {
    return false;
  }
  if (*end != '\0' && *end != ',' && *end != '}' &&
      *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
    return false;
  }

  api_id = static_cast<int>(value);
  return true;
}

static std::string json_quote(const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  std::string result;
  result.push_back('"');

  for (std::string::const_iterator position = value.begin();
       position != value.end(); ++position) {
    const unsigned char ch = static_cast<unsigned char>(*position);
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

static bool send_request(void *client, const std::string &request) {
  td_json_client_send(client, request.c_str());
  return true;
}

int main() {
  std::ifstream secrets_file("secrets/telegram.json");
  if (!secrets_file) {
    std::fprintf(stderr, "Cannot open secrets/telegram.json\n");
    return 1;
  }

  std::ostringstream secrets_stream;
  secrets_stream << secrets_file.rdbuf();
  const std::string secrets = secrets_stream.str();

  int api_id = 0;
  std::string api_hash;
  if (!read_api_id(secrets.c_str(), api_id) ||
      !find_string_member(secrets.c_str(), "api_hash", api_hash) ||
      api_hash.empty()) {
    std::fprintf(stderr, "Invalid api_id or api_hash in secrets/telegram.json\n");
    return 1;
  }

  char database_directory[128];
  std::snprintf(database_directory, sizeof(database_directory),
                "/tmp/tdlogin_%ld", static_cast<long>(getpid()));
  if (mkdir(database_directory, 0700) != 0) {
    std::fprintf(stderr, "Cannot create fresh TDLib database directory: %s\n",
                 std::strerror(errno));
    return 1;
  }

  void *client = td_json_client_create();
  if (client == NULL) {
    std::fprintf(stderr, "Cannot create TDLib client\n");
    return 1;
  }

  const std::string directory = database_directory;
  std::ostringstream parameters;
  parameters << "{\"@type\":\"setTdlibParameters\","
             << "\"use_test_dc\":true,"
             << "\"api_id\":" << api_id << ","
             << "\"api_hash\":" << json_quote(api_hash) << ","
             << "\"database_directory\":" << json_quote(directory) << ","
             << "\"files_directory\":" << json_quote(directory) << ","
             << "\"use_file_database\":false,"
             << "\"use_chat_info_database\":false,"
             << "\"use_message_database\":false,"
             << "\"use_secret_chats\":false,"
             << "\"system_language_code\":\"en\","
             << "\"device_model\":\"PC\","
             << "\"system_version\":\"Linux\","
             << "\"application_version\":\"0.1\"}";
  send_request(client, parameters.str());

  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool phone_number_sent = false;
  bool code_sent = false;
  bool ready = false;
  bool failed = false;

  while (std::chrono::steady_clock::now() < deadline) {
    const std::chrono::duration<double> remaining =
        deadline - std::chrono::steady_clock::now();
    const double timeout = remaining.count() < 1.0 ? remaining.count() : 1.0;
    const char *response = td_json_client_receive(client, timeout);
    if (response == NULL) {
      continue;
    }

    std::string response_type;
    find_string_member(response, "@type", response_type);

    if (response_type == "error") {
      std::string message;
      find_string_member(response, "message", message);
      std::printf("%s: %s\n", response_type.c_str(), message.c_str());
      std::fflush(stdout);
      failed = true;
      break;
    }

    const char *authorization_state =
        find_member_value(response, "authorization_state");
    std::string state_type;
    if (authorization_state != NULL) {
      find_string_member(authorization_state, "@type", state_type);
    } else if (response_type.compare(0, 18, "authorizationState") == 0) {
      state_type = response_type;
    }

    if (state_type.empty()) {
      continue;
    }

    std::printf("%s\n", state_type.c_str());
    std::fflush(stdout);

    if (state_type == "authorizationStateWaitPhoneNumber" &&
        !phone_number_sent) {
      send_request(client,
                   "{\"@type\":\"setAuthenticationPhoneNumber\","
                   "\"phone_number\":\"9996621234\"}");
      phone_number_sent = true;
    } else if (state_type == "authorizationStateWaitCode" && !code_sent) {
      send_request(client,
                   "{\"@type\":\"checkAuthenticationCode\","
                   "\"code\":\"22222\"}");
      code_sent = true;
    } else if (state_type == "authorizationStateReady") {
      ready = true;
      break;
    }
  }

  td_json_client_destroy(client);
  if (ready) {
    return 0;
  }
  return failed ? 1 : 1;
}
