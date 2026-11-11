#!/usr/bin/env ruby
# genpreferences.rb <UnifiedWebPreferences.yaml> <out.cpp> — WebKit's defaults for WebCore::Settings.
#
# WebCore's own defaults are deliberately neutral (JavaScript off, ...): every WebKit browser applies its embedder
# defaults ("WebKit" column of UnifiedWebPreferences.yaml) on top. This emits one applyWebKitDefaults(Settings&)
# with those values; the yaml's conditions stay #if, so our build picks what GTK/WPE pick on equal terms.
# Values that call WebKit-internal functions (defaultSomething()) can't compile here: WebCore's value is kept and
# the preference is listed at the end of the file, to be decided by hand in WebPage.cpp.
require 'yaml'

input, output = ARGV
abort "usage: genpreferences.rb <UnifiedWebPreferences.yaml> <out.cpp>" unless input && output
prefs = YAML.load_file(input)

def webcore_name(name, options)
  return options["webcoreName"] if options["webcoreName"]
  return name[0..1].downcase + name[2..] if name.start_with?("VP")
  return name if name.start_with?("CSSOM", "HTTPS")
  return name[0..2].downcase + name[3..] if name.start_with?("URL", "CSS", "XSS", "FTP", "DOM", "DNS", "PDF", "ICE", "HDR", "RTC", "SVG")
  return name[0..3].downcase + name[4..] if name.start_with?("HTTP", "HTML")
  name[0].downcase + name[1..]
end

def setter(n)
  return "set" + n[0..3].upcase + n[4..] if n.start_with?("html")
  return "set" + n[0..2].upcase + n[3..] if n.start_with?("url", "css", "xss", "ftp", "dom", "dns", "ice", "hdr", "pdf", "rtc", "svg")
  return "set" + n[0..1].upcase + n[2..] if n.start_with?("vp")
  "set" + n[0].upcase + n[1..]
end

# What WebKit's own default functions (Source/WebKit/Shared/WebPreferencesDefaultValues.cpp) return on non-Cocoa
# platforms, for the ones used as the unconditional default. Others are listed at the end of the output.
NON_COCOA_VALUES = {
  "WebKit::defaultPopoverAttributeEnabled()" => "true",
  "WebKit::defaultTrustedTypesEnabled()" => "true",
  # ENABLE_COOKIE_STORE_API_BY_DEFAULT is on for GTK/WPE (PlatformEnableGlib.h); we follow them.
  "defaultCookieStoreAPIEnabled()" => "true",
  "WebKit::defaultFontFaceSetConstructorEnabled()" => "false",
  "WebKit::defaultMutationEventsEnabled()" => "false",
}

# nil: not expressible in WebCore terms. compiled: whether the branch is compiled in our build (false for branches
# under conditions such as PLATFORM(COCOA): their WebKit-only calls are never seen by the compiler).
PRIMITIVE_TYPES = %w[bool double float int uint32_t int32_t uint64_t unsigned String]

def literal(value, type, compiled = true, refined = nil)
  case value
  when true, false then value.to_s
  when Integer, Float
    return "static_cast<#{refined}>(#{value})" if refined
    return "#{type}(#{value}u)" unless PRIMITIVE_TYPES.include?(type)
    value.to_s
  when String
    if value =~ /\A[A-Za-z_][A-Za-z0-9_:]*\(.*\)\z/ && !value.start_with?("WebCore::", "PAL::")
      return NON_COCOA_VALUES[value] if NON_COCOA_VALUES[value]
      return compiled ? nil : value
    end
    return "static_cast<#{refined}>(#{value})" if refined
    return value if type != "String" || value.start_with?("PAL::", "WebCore::")
    "String(" + value + ")"
  else nil
  end
end

# Branches that certainly don't apply to the WP81 port.
# A branch requires an Apple platform when one of its AND terms is PLATFORM(COCOA/IOS...) (no ORs, not negated).
def apple_only?(condition)
  return true if condition.include?("LIBWEBRTC")
  return false if condition.include?("||")
  condition.split("&&").any? { |term| term.strip =~ /\A\(?PLATFORM\((COCOA|IOS_FAMILY|MAC|WATCHOS|VISION|IOS_FAMILY_SIMULATOR|APPLETV)\)\)?\z/ }
end

lines = []
skipped = []
prefs.each do |name, options|
  next if options["webcoreBinding"]
  defaults = options["defaultValue"] || {}
  webkit = defaults["WebKit"]
  webcore = defaults["WebCore"]
  next unless webkit && webcore
  next if options["webcoreImplementation"] == "custom"
  type = options["type"]
  refined = options["refinedType"]
  call = setter(webcore_name(name, options))
  branches = webkit.map { |condition, value| [condition, literal(value, type, condition == "default" || !apple_only?(condition), refined)] }
  if branches.any? { |_, v| v.nil? }
    skipped << name
    next
  end
  # Nothing to do where WebKit's and WebCore's values agree everywhere.
  next if branches.all? { |c, v| v == literal(webcore[c] || webcore["default"], type, false, refined) }
  lines << "#if #{options["condition"]}" if options["condition"]
  if branches.length == 1
    lines << "    settings.#{call}(#{branches[0][1]});"
  else
    first = true
    branches.each do |condition, value|
      next if condition == "default"
      lines << "#{first ? "#if" : "#elif"} #{condition}"
      lines << "    settings.#{call}(#{value});"
      first = false
    end
    fallback = branches.find { |c, _| c == "default" }
    lines << "#else" << "    settings.#{call}(#{fallback[1]});" if fallback
    lines << "#endif"
  end
  lines << "#endif" if options["condition"]
end

File.write(output, <<~CPP)
  // Generated by engine/tools/genpreferences.rb from #{File.basename(input)}: do not edit.
  #include "config.h"
  #include "WebPreferencesDefaults.h"

  #include <WebCore/Settings.h>
  #include <pal/text/TextEncodingRegistry.h>

  namespace WebKit {

  void applyWebKitDefaults(WebCore::Settings& settings)
  {
  #{lines.join("\n")}
  }

  } // namespace WebKit

  // Kept at WebCore's value (WebKit computes these with its own functions): #{skipped.join(", ")}
CPP
puts "#{lines.count { |l| l.include?("settings.") }} settings, #{skipped.length} kept at WebCore's value"
