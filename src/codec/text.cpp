// Canonical text rendering and strict parsing of pod documents.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "podfabric/codec/codec.hpp"
#include "podfabric/core/bytes.hpp"
#include "podfabric/core/checked.hpp"
#include "podfabric/version.hpp"

namespace podfabric::codec {
namespace {

constexpr std::size_t kMaxLineBytes = 8192;
constexpr std::size_t kMaxLines = 1u << 20;
constexpr std::size_t kMaxTextItems = 1u << 20;

std::vector<std::string_view> split_tokens(std::string_view line) {
  std::vector<std::string_view> tokens;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) {
      tokens.push_back(line.substr(start, i - start));
    }
  }
  return tokens;
}

std::string_view blank_to_empty(std::string_view token) {
  return token == "-" ? std::string_view{} : token;
}

Result<std::uint64_t> parse_u64(std::string_view token, const char* what) {
  if (token.empty() || token.size() > 20) {
    return Status(Code::Invalid, std::string("field '") + what + "' is not an unsigned integer");
  }
  std::uint64_t value = 0;
  for (char c : token) {
    if (c < '0' || c > '9') {
      return Status(Code::Invalid, std::string("field '") + what + "' has a non-digit character");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) {
      return Status(Code::Invalid, std::string("field '") + what + "' overflows 64 bits");
    }
    value = value * 10ull + digit;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view token, const char* what) {
  bool negative = false;
  if (!token.empty() && (token.front() == '-' || token.front() == '+')) {
    negative = token.front() == '-';
    token.remove_prefix(1);
  }
  PODFABRIC_TRY_ASSIGN(const std::uint64_t magnitude, parse_u64(token, what));
  if (negative) {
    if (magnitude > 0x8000000000000000ull) {
      return Status(Code::Invalid, std::string("field '") + what + "' underflows 64 bits");
    }
    if (magnitude == 0x8000000000000000ull) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
  }
  if (magnitude > 0x7FFFFFFFFFFFFFFFull) {
    return Status(Code::Invalid, std::string("field '") + what + "' overflows 64 bits");
  }
  return static_cast<std::int64_t>(magnitude);
}

Result<std::uint32_t> parse_u32(std::string_view token, const char* what) {
  PODFABRIC_TRY_ASSIGN(const std::uint64_t value, parse_u64(token, what));
  if (value > 0xFFFFFFFFull) {
    return Status(Code::Invalid, std::string("field '") + what + "' exceeds 32 bits");
  }
  return static_cast<std::uint32_t>(value);
}

template <class Enum>
Result<Enum> parse_enum(std::string_view token, const char* what,
                        std::initializer_list<std::pair<std::string_view, Enum>> table) {
  for (const auto& entry : table) {
    if (entry.first == token) {
      return entry.second;
    }
  }
  return Status(Code::Invalid, std::string("field '") + what + "' has an unrecognised value");
}

Result<EvidenceQuality> parse_quality(std::string_view token) {
  return parse_enum<EvidenceQuality>(token, "prov.quality",
                                     {{"REAL", EvidenceQuality::Real},
                                      {"SYNTHETIC", EvidenceQuality::Synthetic},
                                      {"UNKNOWN", EvidenceQuality::Unknown}});
}

Result<EvidenceClass> parse_class(std::string_view token) {
  return parse_enum<EvidenceClass>(token, "prov.class",
                                   {{"attested", EvidenceClass::Attested},
                                    {"observed", EvidenceClass::Observed},
                                    {"declared", EvidenceClass::Declared},
                                    {"reconstructed", EvidenceClass::Reconstructed}});
}

Result<MembershipState> parse_membership(std::string_view token) {
  return parse_enum<MembershipState>(token, "member.state",
                                     {{"joining", MembershipState::Joining},
                                      {"established", MembershipState::Established},
                                      {"leaving", MembershipState::Leaving},
                                      {"fenced", MembershipState::Fenced},
                                      {"retired", MembershipState::Retired}});
}

Result<AdminState> parse_admin(std::string_view token) {
  return parse_enum<AdminState>(token, "admin",
                                {{"enabled", AdminState::Enabled},
                                 {"disabled", AdminState::Disabled},
                                 {"maintenance", AdminState::Maintenance},
                                 {"draining", AdminState::Draining}});
}

Result<HealthState> parse_health(std::string_view token) {
  return parse_enum<HealthState>(token, "health",
                                 {{"ok", HealthState::Ok},
                                  {"impaired", HealthState::Impaired},
                                  {"critical", HealthState::Critical},
                                  {"unknown", HealthState::Unknown}});
}

Result<OperationalState> parse_oper(std::string_view token) {
  return parse_enum<OperationalState>(token, "oper",
                                      {{"up", OperationalState::Up},
                                       {"degraded", OperationalState::Degraded},
                                       {"down", OperationalState::Down},
                                       {"unknown", OperationalState::Unknown}});
}

Result<RouteState> parse_route_state(std::string_view token) {
  return parse_enum<RouteState>(token, "route.state",
                                {{"installed", RouteState::Installed},
                                 {"pending", RouteState::Pending},
                                 {"withdrawn", RouteState::Withdrawn},
                                 {"failed", RouteState::Failed},
                                 {"unknown", RouteState::Unknown}});
}

Result<DomainKind> parse_domain_kind(std::string_view token, const char* what) {
  return parse_enum<DomainKind>(token, what,
                                {{"rack", DomainKind::Rack},
                                 {"power", DomainKind::Power},
                                 {"switch-plane", DomainKind::SwitchPlane},
                                 {"row", DomainKind::Row},
                                 {"site", DomainKind::Site},
                                 {"unknown", DomainKind::Unknown}});
}

Result<ObligationKind> parse_obligation_kind(std::string_view token) {
  return parse_enum<ObligationKind>(token, "obligation.kind",
                                    {{"protected-path", ObligationKind::ProtectedPath},
                                     {"capacity-floor", ObligationKind::CapacityFloor},
                                     {"domain-diversity", ObligationKind::DomainDiversity},
                                     {"maintenance-window", ObligationKind::MaintenanceWindow}});
}

std::string escape_text(std::string_view text) {
  // Free text is written verbatim on a continuation line. Newlines and control
  // characters would break the line grammar, so they are rejected at encode
  // time rather than silently transformed.
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string id_or_dash(std::string_view token) { return token.empty() ? "-" : std::string(token); }

std::string prov_inline(const Provenance& provenance) {
  std::string out(to_string(provenance.quality));
  out.push_back(' ');
  out.append(to_string(provenance.klass));
  out.push_back(' ');
  out.append(id_or_dash(provenance.source.token()));
  out.push_back(' ');
  out.append(std::to_string(provenance.source_sequence));
  return out;
}

struct LineBuilder {
  std::string out;
  void push_back_newline() { out.push_back('\n'); }
  void line(std::string_view text) {
    out.append(text);
    out.push_back('\n');
  }
  // Key/value header lines are newline terminated; record bodies are appended
  // directly by their writers.
  void kv(std::string_view key, std::string_view value) {
    out.append(key);
    out.push_back(' ');
    out.append(value);
    out.push_back('\n');
  }
  void num(std::string_view key, std::uint64_t value) { kv(key, std::to_string(value)); }
  void i64(std::string_view key, std::int64_t value) { kv(key, std::to_string(value)); }
};

class TextParser {
 public:
  explicit TextParser(std::string_view text) : text_(text) {}

  Result<PodSnapshot> parse() {
    std::size_t cursor = 0;
    std::size_t line_number = 0;
    bool saw_header = false;
    bool saw_end = false;
    while (cursor <= text_.size()) {
      const std::size_t newline = text_.find('\n', cursor);
      const std::size_t end = newline == std::string_view::npos ? text_.size() : newline;
      std::string_view line = text_.substr(cursor, end - cursor);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      cursor = end + 1;
      ++line_number;
      if (line_number > kMaxLines) {
        return Status(Code::Invalid, "text document has too many lines");
      }
      if (line.size() > kMaxLineBytes) {
        return Status(Code::Invalid, "text document has an oversized line");
      }
      if (!is_valid_utf8(line)) {
        return Status(Code::Invalid, "text document contains invalid UTF-8");
      }
      const auto tokens = split_tokens(line);
      if (tokens.empty()) {
        if (end == text_.size()) break;
        continue;
      }
      if (!saw_header) {
        if (tokens[0] != text_schema) {
          return Status(Code::Unsupported, "unrecognised document header");
        }
        saw_header = true;
        snapshot_.schema.assign(std::string(snapshot_schema));
        if (end == text_.size()) break;
        continue;
      }
      if (tokens[0] == "end") {
        saw_end = true;
        std::string_view remainder = text_.substr(cursor);
        for (char c : remainder) {
          if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return Status(Code::Invalid, "content follows the end marker");
          }
        }
        break;
      }
      PODFABRIC_TRY(handle_line(tokens));
      if (end == text_.size()) break;
    }
    if (!saw_header) {
      return Status(Code::Invalid, "text document has no header line");
    }
    if (!saw_end) {
      return Status(Code::Incomplete, "text document has no end marker");
    }
    return snapshot_;
  }

 private:
  Status handle_line(const std::vector<std::string_view>& tokens) {
    const std::string_view directive = tokens[0];
    if (directive == "pod") {
      if (tokens.size() != 2) return Status(Code::Invalid, "pod directive takes one argument");
      PODFABRIC_TRY_ASSIGN(snapshot_.pod, PodId::parse(blank_to_empty(tokens[1])));
      return Status::success();
    }
    if (directive == "schema") {
      if (tokens.size() != 2) return Status(Code::Invalid, "schema directive takes one argument");
      snapshot_.schema.assign(tokens[1]);
      return Status::success();
    }
    if (directive == "observed-at") {
      if (tokens.size() != 2) return Status(Code::Invalid, "observed-at takes one argument");
      PODFABRIC_TRY_ASSIGN(snapshot_.observed_at, parse_i64(tokens[1], "observed-at"));
      return Status::success();
    }
    if (directive == "member") return parse_member(tokens);
    if (directive == "link") return parse_link(tokens);
    if (directive == "route") return parse_route(tokens);
    if (directive == "fdom") return parse_domain(tokens);
    if (directive == "obligation") return parse_obligation(tokens);
    if (directive == "prov") {
      PODFABRIC_TRY_ASSIGN(snapshot_.provenance, parse_prov(tokens, 1));
      current_ = Current::Snapshot;
      return Status::success();
    }
    if (directive[0] == '+') return parse_continuation(directive, tokens);
    return Status(Code::Invalid, "unrecognised directive '" + std::string(directive) + "'");
  }

  // prov <quality> <class> <source|-> <sequence>
  Result<Provenance> parse_prov(const std::vector<std::string_view>& tokens, std::size_t at) {
    if (tokens.size() < at + 4) {
      return Status(Code::Invalid, "provenance needs quality, class, source and sequence");
    }
    Provenance provenance;
    PODFABRIC_TRY_ASSIGN(provenance.quality, parse_quality(tokens[at]));
    PODFABRIC_TRY_ASSIGN(provenance.klass, parse_class(tokens[at + 1]));
    const std::string_view source = blank_to_empty(tokens[at + 2]);
    if (!source.empty()) {
      PODFABRIC_TRY_ASSIGN(provenance.source, NodeId::parse(source));
    }
    PODFABRIC_TRY_ASSIGN(provenance.source_sequence, parse_u64(tokens[at + 3], "prov.sequence"));
    return provenance;
  }

  static bool contains(const std::vector<std::string_view>& tokens, std::size_t at,
                       std::string_view keyword, std::size_t needed) {
    return tokens.size() >= at + needed && tokens[at] == keyword;
  }

  Status parse_member(const std::vector<std::string_view>& tokens) {
    if (snapshot_.members.size() >= kMaxTextItems) {
      return Status(Code::Exhausted, "too many member records");
    }
    if (tokens.size() < 20) {
      return Status(Code::Invalid, "member directive is missing fields");
    }
    MemberRecord member;
    PODFABRIC_TRY_ASSIGN(member.rack, RackId::parse(blank_to_empty(tokens[1])));
    if (!contains(tokens, 2, "gen", 2)) return Status(Code::Invalid, "member needs gen");
    PODFABRIC_TRY_ASSIGN(const std::uint64_t generation, parse_u64(tokens[3], "member.gen"));
    member.generation = RackGeneration(generation);
    if (!contains(tokens, 4, "digest", 2)) return Status(Code::Invalid, "member needs digest");
    PODFABRIC_TRY_ASSIGN(member.digest, Digest::parse(tokens[5]));
    if (!contains(tokens, 6, "rnf", 2)) return Status(Code::Invalid, "member needs rnf");
    member.rnf_schema.assign(blank_to_empty(tokens[7]));
    if (!contains(tokens, 8, "state", 2)) return Status(Code::Invalid, "member needs state");
    PODFABRIC_TRY_ASSIGN(member.membership, parse_membership(tokens[9]));
    if (!contains(tokens, 10, "admin", 2)) return Status(Code::Invalid, "member needs admin");
    PODFABRIC_TRY_ASSIGN(member.admin, parse_admin(tokens[11]));
    if (!contains(tokens, 12, "health", 2)) return Status(Code::Invalid, "member needs health");
    PODFABRIC_TRY_ASSIGN(member.health, parse_health(tokens[13]));
    if (!contains(tokens, 14, "observed", 2)) return Status(Code::Invalid, "member needs observed");
    PODFABRIC_TRY_ASSIGN(member.observed_at, parse_i64(tokens[15], "member.observed"));
    if (!contains(tokens, 16, "prov", 4)) return Status(Code::Invalid, "member needs prov");
    PODFABRIC_TRY_ASSIGN(member.provenance, parse_prov(tokens, 17));
    if (tokens.size() > 21) {
      return Status(Code::Invalid, "member directive has trailing tokens");
    }
    snapshot_.members.push_back(std::move(member));
    current_ = Current::Member;
    return Status::success();
  }

  Status parse_link(const std::vector<std::string_view>& tokens) {
    if (snapshot_.links.size() >= kMaxTextItems) {
      return Status(Code::Exhausted, "too many link records");
    }
    if (tokens.size() < 31) {
      return Status(Code::Invalid, "link directive is missing fields");
    }
    LinkRecord link;
    PODFABRIC_TRY_ASSIGN(link.id, LinkId::parse(blank_to_empty(tokens[1])));
    if (!contains(tokens, 2, "a", 3)) return Status(Code::Invalid, "link needs a <rack> <port>");
    PODFABRIC_TRY_ASSIGN(link.a.rack, RackId::parse(blank_to_empty(tokens[3])));
    PODFABRIC_TRY_ASSIGN(link.a.port, PortRef::parse(blank_to_empty(tokens[4])));
    if (!contains(tokens, 5, "b", 3)) return Status(Code::Invalid, "link needs b <rack> <port>");
    PODFABRIC_TRY_ASSIGN(link.b.rack, RackId::parse(blank_to_empty(tokens[6])));
    PODFABRIC_TRY_ASSIGN(link.b.port, PortRef::parse(blank_to_empty(tokens[7])));
    if (!contains(tokens, 8, "agen", 2)) return Status(Code::Invalid, "link needs agen");
    PODFABRIC_TRY_ASSIGN(const std::uint64_t agen, parse_u64(tokens[9], "link.agen"));
    link.a_generation = RackGeneration(agen);
    if (!contains(tokens, 10, "bgen", 2)) return Status(Code::Invalid, "link needs bgen");
    PODFABRIC_TRY_ASSIGN(const std::uint64_t bgen, parse_u64(tokens[11], "link.bgen"));
    link.b_generation = RackGeneration(bgen);
    if (!contains(tokens, 12, "domain", 2)) return Status(Code::Invalid, "link needs domain");
    PODFABRIC_TRY_ASSIGN(link.domain, DomainId::parse(blank_to_empty(tokens[13])));
    if (!contains(tokens, 14, "resource", 2)) return Status(Code::Invalid, "link needs resource");
    PODFABRIC_TRY_ASSIGN(link.resource, ResourceClass::parse(blank_to_empty(tokens[15])));
    if (!contains(tokens, 16, "capacity", 2)) return Status(Code::Invalid, "link needs capacity");
    PODFABRIC_TRY_ASSIGN(link.capacity, parse_u64(tokens[17], "link.capacity"));
    if (!contains(tokens, 18, "admin", 2)) return Status(Code::Invalid, "link needs admin");
    PODFABRIC_TRY_ASSIGN(link.admin, parse_admin(tokens[19]));
    if (!contains(tokens, 20, "oper", 2)) return Status(Code::Invalid, "link needs oper");
    PODFABRIC_TRY_ASSIGN(link.oper, parse_oper(tokens[21]));
    if (!contains(tokens, 22, "route", 2)) return Status(Code::Invalid, "link needs route");
    const std::string_view route = blank_to_empty(tokens[23]);
    if (!route.empty()) {
      PODFABRIC_TRY_ASSIGN(link.route, RouteRef::parse(route));
    }
    if (!contains(tokens, 24, "observed", 2)) return Status(Code::Invalid, "link needs observed");
    PODFABRIC_TRY_ASSIGN(link.observed_at, parse_i64(tokens[25], "link.observed"));
    if (!contains(tokens, 26, "prov", 4)) return Status(Code::Invalid, "link needs prov");
    PODFABRIC_TRY_ASSIGN(link.provenance, parse_prov(tokens, 27));
    if (tokens.size() > 31) {
      return Status(Code::Invalid, "link directive has trailing tokens");
    }
    snapshot_.links.push_back(std::move(link));
    current_ = Current::Link;
    return Status::success();
  }

  Status parse_route(const std::vector<std::string_view>& tokens) {
    if (snapshot_.routes.size() >= kMaxTextItems) {
      return Status(Code::Exhausted, "too many route records");
    }
    if (tokens.size() < 20) {
      return Status(Code::Invalid, "route directive is missing fields");
    }
    RouteEvidence route;
    PODFABRIC_TRY_ASSIGN(route.ref, RouteRef::parse(blank_to_empty(tokens[1])));
    if (!contains(tokens, 2, "gen", 2)) return Status(Code::Invalid, "route needs gen");
    PODFABRIC_TRY_ASSIGN(const std::uint64_t generation, parse_u64(tokens[3], "route.gen"));
    route.generation = RouteGeneration(generation);
    if (!contains(tokens, 4, "digest", 2)) return Status(Code::Invalid, "route needs digest");
    PODFABRIC_TRY_ASSIGN(route.authority_digest, Digest::parse(tokens[5]));
    if (!contains(tokens, 6, "resource", 2)) return Status(Code::Invalid, "route needs resource");
    PODFABRIC_TRY_ASSIGN(route.resource, ResourceClass::parse(blank_to_empty(tokens[7])));
    if (!contains(tokens, 8, "capacity", 2)) return Status(Code::Invalid, "route needs capacity");
    PODFABRIC_TRY_ASSIGN(route.committed_capacity, parse_u64(tokens[9], "route.capacity"));
    if (!contains(tokens, 10, "state", 2)) return Status(Code::Invalid, "route needs state");
    PODFABRIC_TRY_ASSIGN(route.state, parse_route_state(tokens[11]));
    if (!contains(tokens, 12, "observed", 2)) return Status(Code::Invalid, "route needs observed");
    PODFABRIC_TRY_ASSIGN(route.observed_at, parse_i64(tokens[13], "route.observed"));
    if (!contains(tokens, 14, "prov", 4)) return Status(Code::Invalid, "route needs prov");
    PODFABRIC_TRY_ASSIGN(route.provenance, parse_prov(tokens, 15));
    if (tokens.size() > 19) {
      return Status(Code::Invalid, "route directive has trailing tokens");
    }
    snapshot_.routes.push_back(std::move(route));
    current_ = Current::Route;
    return Status::success();
  }

  Status parse_domain(const std::vector<std::string_view>& tokens) {
    if (snapshot_.domains.size() >= kMaxTextItems) {
      return Status(Code::Exhausted, "too many failure-domain records");
    }
    if (tokens.size() < 8) {
      return Status(Code::Invalid, "fdom directive is missing fields");
    }
    FailureDomainRecord domain;
    PODFABRIC_TRY_ASSIGN(domain.id, DomainId::parse(blank_to_empty(tokens[1])));
    if (!contains(tokens, 2, "kind", 2)) return Status(Code::Invalid, "fdom needs kind");
    PODFABRIC_TRY_ASSIGN(domain.kind, parse_domain_kind(tokens[3], "fdom.kind"));
    if (!contains(tokens, 4, "prov", 4)) return Status(Code::Invalid, "fdom needs prov");
    PODFABRIC_TRY_ASSIGN(domain.provenance, parse_prov(tokens, 5));
    if (tokens.size() > 9) {
      domain.provenance.detail.assign(rest_of_line(tokens, 9));
    }
    snapshot_.domains.push_back(std::move(domain));
    current_ = Current::Domain;
    return Status::success();
  }

  Status parse_obligation(const std::vector<std::string_view>& tokens) {
    if (snapshot_.obligations.size() >= kMaxTextItems) {
      return Status(Code::Exhausted, "too many obligation records");
    }
    if (tokens.size() < 16) {
      return Status(Code::Invalid, "obligation directive is missing fields");
    }
    Obligation obligation;
    PODFABRIC_TRY_ASSIGN(obligation.id, ObligationId::parse(blank_to_empty(tokens[1])));
    if (!contains(tokens, 2, "kind", 2)) return Status(Code::Invalid, "obligation needs kind");
    PODFABRIC_TRY_ASSIGN(obligation.kind, parse_obligation_kind(tokens[3]));
    if (!contains(tokens, 4, "tenant", 2)) return Status(Code::Invalid, "obligation needs tenant");
    const std::string_view tenant = blank_to_empty(tokens[5]);
    if (!tenant.empty()) {
      PODFABRIC_TRY_ASSIGN(obligation.tenant, TenantId::parse(tenant));
    }
    if (!contains(tokens, 6, "mandatory", 2)) {
      return Status(Code::Invalid, "obligation needs mandatory");
    }
    PODFABRIC_TRY_ASSIGN(const std::uint32_t mandatory, parse_u32(tokens[7], "obligation.mandatory"));
    if (mandatory > 1) return Status(Code::Invalid, "obligation.mandatory is not 0 or 1");
    obligation.mandatory = mandatory == 1;
    if (!contains(tokens, 8, "domains", 2)) return Status(Code::Invalid, "obligation needs domains");
    PODFABRIC_TRY_ASSIGN(obligation.required_domains, parse_u32(tokens[9], "obligation.domains"));
    if (!contains(tokens, 10, "diversity", 2)) {
      return Status(Code::Invalid, "obligation needs diversity");
    }
    PODFABRIC_TRY_ASSIGN(obligation.diversity_kind,
                         parse_domain_kind(tokens[11], "obligation.diversity"));
    if (!contains(tokens, 12, "prov", 4)) return Status(Code::Invalid, "obligation needs prov");
    PODFABRIC_TRY_ASSIGN(obligation.provenance, parse_prov(tokens, 13));
    if (tokens.size() > 17) {
      obligation.provenance.detail.assign(rest_of_line(tokens, 17));
    }
    snapshot_.obligations.push_back(std::move(obligation));
    current_ = Current::Obligation;
    return Status::success();
  }

  Status parse_continuation(std::string_view directive,
                            const std::vector<std::string_view>& tokens) {
    if (current_ == Current::None) {
      return Status(Code::Invalid, "continuation line has no preceding record");
    }
    if (directive == "+detail" || directive == "+desc") {
      const std::string_view rest = rest_of_line(tokens);
      switch (current_) {
        case Current::Snapshot:
          if (directive == "+detail") snapshot_.provenance.detail.assign(rest);
          break;
        case Current::Member:
          if (directive == "+desc") snapshot_.members.back().provenance.detail.assign(rest);
          break;
        case Current::Link:
          if (directive == "+desc") snapshot_.links.back().provenance.detail.assign(rest);
          break;
        case Current::Route:
          if (directive == "+desc") snapshot_.routes.back().provenance.detail.assign(rest);
          break;
        case Current::Domain:
          if (directive == "+desc") snapshot_.domains.back().description.assign(rest);
          break;
        case Current::Obligation:
          if (directive == "+desc") snapshot_.obligations.back().description.assign(rest);
          break;
        case Current::None:
          break;
      }
      return Status::success();
    }
    switch (current_) {
      case Current::Member: {
        if (directive == "+domain") {
          if (tokens.size() != 2) return Status(Code::Invalid, "+domain takes one argument");
          PODFABRIC_TRY_ASSIGN(const DomainId id, DomainId::parse(blank_to_empty(tokens[1])));
          snapshot_.members.back().failure_domains.push_back(id);
          return Status::success();
        }
        if (directive == "+port") {
          if (tokens.size() != 2) return Status(Code::Invalid, "+port takes one argument");
          PODFABRIC_TRY_ASSIGN(const PortRef port, PortRef::parse(blank_to_empty(tokens[1])));
          snapshot_.members.back().ports.push_back(port);
          return Status::success();
        }
        if (directive == "+capacity") {
          if (tokens.size() < 10) {
            return Status(Code::Invalid, "+capacity takes nine arguments and a provenance");
          }
          CapacityClaim claim;
          PODFABRIC_TRY_ASSIGN(claim.resource, ResourceClass::parse(blank_to_empty(tokens[1])));
          PODFABRIC_TRY_ASSIGN(claim.exclusivity, ExclusivityKey::parse(blank_to_empty(tokens[2])));
          PODFABRIC_TRY_ASSIGN(claim.amount, parse_u64(tokens[3], "capacity.amount"));
          PODFABRIC_TRY_ASSIGN(claim.reserved, parse_u64(tokens[4], "capacity.reserved"));
          PODFABRIC_TRY_ASSIGN(claim.domain, DomainId::parse(blank_to_empty(tokens[5])));
          PODFABRIC_TRY_ASSIGN(claim.provenance, parse_prov(tokens, 6));
          if (tokens.size() > 10) {
            claim.provenance.detail.assign(rest_of_line(tokens, 10));
          }
          snapshot_.members.back().capacity.push_back(claim);
          return Status::success();
        }
        break;
      }
      case Current::Route: {
        if (directive == "+hop") {
          if (tokens.size() != 3) return Status(Code::Invalid, "+hop takes two arguments");
          PODFABRIC_TRY_ASSIGN(const RackId rack, RackId::parse(blank_to_empty(tokens[1])));
          PODFABRIC_TRY_ASSIGN(const std::uint64_t generation, parse_u64(tokens[2], "hop.gen"));
          snapshot_.routes.back().path.push_back(rack);
          snapshot_.routes.back().path_generations.push_back(RackGeneration(generation));
          return Status::success();
        }
        break;
      }
      case Current::Domain: {
        if (directive == "+member") {
          if (tokens.size() != 2) return Status(Code::Invalid, "+member takes one argument");
          PODFABRIC_TRY_ASSIGN(const RackId rack, RackId::parse(blank_to_empty(tokens[1])));
          snapshot_.domains.back().members.push_back(rack);
          return Status::success();
        }
        break;
      }
      case Current::Obligation: {
        if (directive == "+rack") {
          if (tokens.size() != 2) return Status(Code::Invalid, "+rack takes one argument");
          PODFABRIC_TRY_ASSIGN(const RackId rack, RackId::parse(blank_to_empty(tokens[1])));
          snapshot_.obligations.back().protected_racks.push_back(rack);
          return Status::success();
        }
        if (directive == "+require") {
          if (tokens.size() != 4) return Status(Code::Invalid, "+require takes three arguments");
          CapacityRequirement requirement;
          PODFABRIC_TRY_ASSIGN(requirement.resource,
                               ResourceClass::parse(blank_to_empty(tokens[1])));
          PODFABRIC_TRY_ASSIGN(requirement.minimum, parse_u64(tokens[2], "require.minimum"));
          PODFABRIC_TRY_ASSIGN(requirement.margin_percent,
                               parse_u32(tokens[3], "require.margin"));
          snapshot_.obligations.back().requirements.push_back(requirement);
          return Status::success();
        }
        break;
      }
      case Current::Snapshot:
      case Current::Link:
      case Current::None:
        break;
    }
    return Status(Code::Invalid, "unrecognised continuation '" + std::string(directive) + "'");
  }

  static std::string_view rest_of_line(const std::vector<std::string_view>& tokens,
                                       std::size_t from = 1) {
    if (tokens.size() <= from) {
      return {};
    }
    // Reconstruct the free text by spanning from the first character after the
    // directive to the end of the last token; interior spacing is normalised.
    std::string_view first = tokens[from];
    std::string_view last = tokens.back();
    if (first.data() == nullptr || last.data() == nullptr) {
      return {};
    }
    const char* begin = first.data();
    const char* end = last.data() + last.size();
    if (end <= begin) {
      return {};
    }
    return std::string_view(begin, static_cast<std::size_t>(end - begin));
  }

  enum class Current { None, Snapshot, Member, Link, Route, Domain, Obligation };

  std::string_view text_;
  PodSnapshot snapshot_{};
  Current current_{Current::None};
};

}  // namespace

Result<std::string> to_text(const PodSnapshot& snapshot) {
  LineBuilder builder;
  // The first line always names the exchange format. The revision of the
  // snapshot document itself travels on its own directive so that both can
  // evolve independently.
  builder.line(text_schema);
  builder.kv("schema", snapshot.schema.empty() ? snapshot_schema : std::string_view(snapshot.schema));
  builder.kv("pod", id_or_dash(snapshot.pod.token()));
  builder.i64("observed-at", snapshot.observed_at);
  builder.out.append("prov ");
  builder.out.append(prov_inline(snapshot.provenance));
  builder.out.push_back('\n');
  if (!snapshot.provenance.detail.empty()) {
    builder.out.append("+detail ");
    builder.out.append(escape_text(snapshot.provenance.detail));
    builder.out.push_back('\n');
  }

  for (const MemberRecord& member : snapshot.members) {
    builder.out.append("member ");
    builder.out.append(id_or_dash(member.rack.token()));
    builder.out.append(" gen ");
    builder.out.append(std::to_string(member.generation.value()));
    builder.out.append(" digest ");
    builder.out.append(member.digest.to_hex());
    builder.out.append(" rnf ");
    builder.out.append(member.rnf_schema.empty() ? "-" : member.rnf_schema);
    builder.out.append(" state ");
    builder.out.append(to_string(member.membership));
    builder.out.append(" admin ");
    builder.out.append(to_string(member.admin));
    builder.out.append(" health ");
    builder.out.append(to_string(member.health));
    builder.out.append(" observed ");
    builder.out.append(std::to_string(member.observed_at));
    builder.out.append(" prov ");
    builder.out.append(prov_inline(member.provenance));
    builder.out.push_back('\n');
    if (!member.provenance.detail.empty()) {
      builder.out.append("+desc ");
      builder.out.append(escape_text(member.provenance.detail));
      builder.out.push_back('\n');
    }
    for (const DomainId& domain : member.failure_domains) {
      builder.out.append("+domain ");
      builder.out.append(id_or_dash(domain.token()));
      builder.out.push_back('\n');
    }
    for (const PortRef& port : member.ports) {
      builder.out.append("+port ");
      builder.out.append(id_or_dash(port.token()));
      builder.out.push_back('\n');
    }
    for (const CapacityClaim& claim : member.capacity) {
      builder.out.append("+capacity ");
      builder.out.append(id_or_dash(claim.resource.token()));
      builder.out.push_back(' ');
      builder.out.append(id_or_dash(claim.exclusivity.token()));
      builder.out.push_back(' ');
      builder.out.append(std::to_string(claim.amount));
      builder.out.push_back(' ');
      builder.out.append(std::to_string(claim.reserved));
      builder.out.push_back(' ');
      builder.out.append(id_or_dash(claim.domain.token()));
      builder.out.push_back(' ');
      builder.out.append(prov_inline(claim.provenance));
      if (!claim.provenance.detail.empty()) {
        builder.out.push_back(' ');
        builder.out.append(escape_text(claim.provenance.detail));
      }
      builder.out.push_back('\n');
    }
  }

  for (const LinkRecord& link : snapshot.links) {
    builder.out.append("link ");
    builder.out.append(id_or_dash(link.id.token()));
    builder.out.append(" a ");
    builder.out.append(id_or_dash(link.a.rack.token()));
    builder.out.push_back(' ');
    builder.out.append(id_or_dash(link.a.port.token()));
    builder.out.append(" b ");
    builder.out.append(id_or_dash(link.b.rack.token()));
    builder.out.push_back(' ');
    builder.out.append(id_or_dash(link.b.port.token()));
    builder.out.append(" agen ");
    builder.out.append(std::to_string(link.a_generation.value()));
    builder.out.append(" bgen ");
    builder.out.append(std::to_string(link.b_generation.value()));
    builder.out.append(" domain ");
    builder.out.append(id_or_dash(link.domain.token()));
    builder.out.append(" resource ");
    builder.out.append(id_or_dash(link.resource.token()));
    builder.out.append(" capacity ");
    builder.out.append(std::to_string(link.capacity));
    builder.out.append(" admin ");
    builder.out.append(to_string(link.admin));
    builder.out.append(" oper ");
    builder.out.append(to_string(link.oper));
    builder.out.append(" route ");
    builder.out.append(id_or_dash(link.route.token()));
    builder.out.append(" observed ");
    builder.out.append(std::to_string(link.observed_at));
    builder.out.append(" prov ");
    builder.out.append(prov_inline(link.provenance));
    builder.out.push_back('\n');
    if (!link.provenance.detail.empty()) {
      builder.out.append("+desc ");
      builder.out.append(escape_text(link.provenance.detail));
      builder.out.push_back('\n');
    }
  }

  for (const RouteEvidence& route : snapshot.routes) {
    builder.out.append("route ");
    builder.out.append(id_or_dash(route.ref.token()));
    builder.out.append(" gen ");
    builder.out.append(std::to_string(route.generation.value()));
    builder.out.append(" digest ");
    builder.out.append(route.authority_digest.to_hex());
    builder.out.append(" resource ");
    builder.out.append(id_or_dash(route.resource.token()));
    builder.out.append(" capacity ");
    builder.out.append(std::to_string(route.committed_capacity));
    builder.out.append(" state ");
    builder.out.append(to_string(route.state));
    builder.out.append(" observed ");
    builder.out.append(std::to_string(route.observed_at));
    builder.out.append(" prov ");
    builder.out.append(prov_inline(route.provenance));
    builder.out.push_back('\n');
    if (!route.provenance.detail.empty()) {
      builder.out.append("+desc ");
      builder.out.append(escape_text(route.provenance.detail));
      builder.out.push_back('\n');
    }
    for (std::size_t i = 0; i < route.path.size(); ++i) {
      builder.out.append("+hop ");
      builder.out.append(id_or_dash(route.path[i].token()));
      builder.out.push_back(' ');
      builder.out.append(std::to_string(i < route.path_generations.size()
                                            ? route.path_generations[i].value()
                                            : 0));
      builder.out.push_back('\n');
    }
  }

  for (const FailureDomainRecord& domain : snapshot.domains) {
    builder.out.append("fdom ");
    builder.out.append(id_or_dash(domain.id.token()));
    builder.out.append(" kind ");
    builder.out.append(to_string(domain.kind));
    builder.out.append(" prov ");
    builder.out.append(prov_inline(domain.provenance));
    if (!domain.provenance.detail.empty()) {
      builder.out.push_back(' ');
      builder.out.append(escape_text(domain.provenance.detail));
    }
    builder.out.push_back('\n');
    if (!domain.description.empty()) {
      builder.out.append("+desc ");
      builder.out.append(escape_text(domain.description));
      builder.out.push_back('\n');
    }
    for (const RackId& rack : domain.members) {
      builder.out.append("+member ");
      builder.out.append(id_or_dash(rack.token()));
      builder.out.push_back('\n');
    }
  }

  for (const Obligation& obligation : snapshot.obligations) {
    builder.out.append("obligation ");
    builder.out.append(id_or_dash(obligation.id.token()));
    builder.out.append(" kind ");
    builder.out.append(to_string(obligation.kind));
    builder.out.append(" tenant ");
    builder.out.append(id_or_dash(obligation.tenant.token()));
    builder.out.append(" mandatory ");
    builder.out.append(obligation.mandatory ? "1" : "0");
    builder.out.append(" domains ");
    builder.out.append(std::to_string(obligation.required_domains));
    builder.out.append(" diversity ");
    builder.out.append(to_string(obligation.diversity_kind));
    builder.out.append(" prov ");
    builder.out.append(prov_inline(obligation.provenance));
    if (!obligation.provenance.detail.empty()) {
      builder.out.push_back(' ');
      builder.out.append(escape_text(obligation.provenance.detail));
    }
    builder.out.push_back('\n');
    if (!obligation.description.empty()) {
      builder.out.append("+desc ");
      builder.out.append(escape_text(obligation.description));
      builder.out.push_back('\n');
    }
    for (const RackId& rack : obligation.protected_racks) {
      builder.out.append("+rack ");
      builder.out.append(id_or_dash(rack.token()));
      builder.out.push_back('\n');
    }
    for (const CapacityRequirement& requirement : obligation.requirements) {
      builder.out.append("+require ");
      builder.out.append(id_or_dash(requirement.resource.token()));
      builder.out.push_back(' ');
      builder.out.append(std::to_string(requirement.minimum));
      builder.out.push_back(' ');
      builder.out.append(std::to_string(requirement.margin_percent));
      builder.out.push_back('\n');
    }
  }

  builder.line("end");
  return builder.out;
}

Result<PodSnapshot> snapshot_from_text(std::string_view text) {
  TextParser parser(text);
  return parser.parse();
}

Result<std::string> to_text(const PodState& state, bool include_decisions) {
  LineBuilder builder;
  builder.line(state.schema.empty() ? pod_state_schema : std::string_view(state.schema));
  builder.kv("pod", id_or_dash(state.pod.token()));
  builder.num("epoch", state.epoch.value());
  builder.kv("incarnation", state.incarnation.to_string());
  builder.out.append("lifecycle ");
  builder.out.append(to_string(state.lifecycle));
  builder.out.push_back('\n');
  builder.out.append("authority ");
  builder.out.append(to_string(state.authority.status));
  builder.out.append(" established ");
  builder.out.append(std::to_string(state.authority.established_members));
  builder.out.append(" fenced ");
  builder.out.append(std::to_string(state.authority.fenced_members));
  builder.out.append(" conflicting ");
  builder.out.append(std::to_string(state.authority.conflicting_members));
  builder.out.append(" stale ");
  builder.out.append(std::to_string(state.authority.stale_members));
  builder.out.append(" recovered ");
  builder.out.append(state.authority.recovered ? "1" : "0");
  builder.out.push_back('\n');
  for (const Reason& reason : state.authority.reasons) {
    builder.out.append("+reason ");
    builder.out.append(to_string(reason.code));
    if (!reason.detail.empty()) {
      builder.out.push_back(' ');
      builder.out.append(escape_text(reason.detail));
    }
    builder.out.push_back('\n');
  }

  for (const MemberVerdict& member : state.members) {
    builder.out.append("member ");
    builder.out.append(id_or_dash(member.rack.token()));
    builder.out.append(" status ");
    builder.out.append(to_string(member.status));
    builder.out.append(" gen ");
    builder.out.append(std::to_string(member.generation.value()));
    builder.out.append(" digest ");
    builder.out.append(member.digest.to_hex());
    builder.out.append(" membership ");
    builder.out.append(to_string(member.membership));
    builder.out.append(" admin ");
    builder.out.append(to_string(member.admin));
    builder.out.append(" health ");
    builder.out.append(to_string(member.health));
    builder.out.push_back('\n');
    for (const Reason& reason : member.reasons) {
      builder.out.append("+reason ");
      builder.out.append(to_string(reason.code));
      if (!reason.detail.empty()) {
        builder.out.push_back(' ');
        builder.out.append(escape_text(reason.detail));
      }
      builder.out.push_back('\n');
    }
    for (const DomainId& domain : member.domains) {
      builder.out.append("+domain ");
      builder.out.append(id_or_dash(domain.token()));
      builder.out.push_back('\n');
    }
  }

  for (const ConnectivityVerdict& link : state.connectivity) {
    builder.out.append("connectivity ");
    builder.out.append(id_or_dash(link.from.token()));
    builder.out.push_back(' ');
    builder.out.append(id_or_dash(link.to.token()));
    builder.out.append(" status ");
    builder.out.append(to_string(link.status));
    builder.out.append(" resource ");
    builder.out.append(id_or_dash(link.resource.token()));
    builder.out.append(" capacity ");
    builder.out.append(std::to_string(link.usable_capacity));
    builder.out.push_back('\n');
    for (const LinkId& id : link.links) {
      builder.out.append("+link ");
      builder.out.append(id_or_dash(id.token()));
      builder.out.push_back('\n');
    }
    for (const RouteRef& ref : link.routes) {
      builder.out.append("+route ");
      builder.out.append(id_or_dash(ref.token()));
      builder.out.push_back('\n');
    }
    for (const Reason& reason : link.reasons) {
      builder.out.append("+reason ");
      builder.out.append(to_string(reason.code));
      if (!reason.detail.empty()) {
        builder.out.push_back(' ');
        builder.out.append(escape_text(reason.detail));
      }
      builder.out.push_back('\n');
    }
  }

  for (const DomainVerdict& domain : state.domains) {
    builder.out.append("domain ");
    builder.out.append(id_or_dash(domain.id.token()));
    builder.out.append(" kind ");
    builder.out.append(to_string(domain.kind));
    builder.out.append(" status ");
    builder.out.append(to_string(domain.status));
    builder.out.append(" declared ");
    builder.out.append(std::to_string(domain.declared_members.size()));
    builder.out.append(" effective ");
    builder.out.append(std::to_string(domain.effective_members.size()));
    builder.out.push_back('\n');
    for (const RackId& rack : domain.declared_members) {
      builder.out.append("+member ");
      builder.out.append(id_or_dash(rack.token()));
      builder.out.push_back('\n');
    }
    for (const Reason& reason : domain.reasons) {
      builder.out.append("+reason ");
      builder.out.append(to_string(reason.code));
      if (!reason.detail.empty()) {
        builder.out.push_back(' ');
        builder.out.append(escape_text(reason.detail));
      }
      builder.out.push_back('\n');
    }
  }

  for (const CapacityAggregate& aggregate : state.capacity) {
    builder.out.append("capacity ");
    builder.out.append(id_or_dash(aggregate.resource.token()));
    builder.out.append(" total ");
    builder.out.append(std::to_string(aggregate.total));
    builder.out.append(" reserved ");
    builder.out.append(std::to_string(aggregate.reserved));
    builder.out.append(" headroom ");
    builder.out.append(std::to_string(aggregate.headroom));
    builder.out.append(" status ");
    builder.out.append(to_string(aggregate.status));
    builder.out.push_back('\n');
    for (const CapacityContribution& contribution : aggregate.contributions) {
      builder.out.append("+contribution ");
      builder.out.append(id_or_dash(contribution.owner.token()));
      builder.out.push_back(' ');
      builder.out.append(id_or_dash(contribution.exclusivity.token()));
      builder.out.push_back(' ');
      builder.out.append(std::to_string(contribution.amount));
      builder.out.push_back(' ');
      builder.out.append(std::to_string(contribution.reserved));
      builder.out.push_back(' ');
      builder.out.append(id_or_dash(contribution.domain.token()));
      builder.out.append(" counted ");
      builder.out.append(contribution.counted ? "1" : "0");
      builder.out.push_back('\n');
    }
    for (const Reason& reason : aggregate.reasons) {
      builder.out.append("+reason ");
      builder.out.append(to_string(reason.code));
      if (!reason.detail.empty()) {
        builder.out.push_back(' ');
        builder.out.append(escape_text(reason.detail));
      }
      builder.out.push_back('\n');
    }
  }

  for (const CapacityAggregate& aggregate : state.domain_capacity) {
    builder.out.append("domain-capacity ");
    builder.out.append(id_or_dash(aggregate.resource.token()));
    builder.out.push_back(' ');
    builder.out.append(id_or_dash(aggregate.domain.token()));
    builder.out.append(" total ");
    builder.out.append(std::to_string(aggregate.total));
    builder.out.append(" reserved ");
    builder.out.append(std::to_string(aggregate.reserved));
    builder.out.append(" headroom ");
    builder.out.append(std::to_string(aggregate.headroom));
    builder.out.append(" status ");
    builder.out.append(to_string(aggregate.status));
    builder.out.push_back('\n');
  }

  for (const ObligationVerdict& obligation : state.obligations) {
    builder.out.append("obligation ");
    builder.out.append(id_or_dash(obligation.id.token()));
    builder.out.append(" kind ");
    builder.out.append(to_string(obligation.kind));
    builder.out.append(" status ");
    builder.out.append(to_string(obligation.status));
    builder.out.append(" satisfied-domains ");
    builder.out.append(std::to_string(obligation.satisfied_domains));
    builder.out.push_back('\n');
    for (const Reason& reason : obligation.reasons) {
      builder.out.append("+reason ");
      builder.out.append(to_string(reason.code));
      if (!reason.detail.empty()) {
        builder.out.push_back(' ');
        builder.out.append(escape_text(reason.detail));
      }
      builder.out.push_back('\n');
    }
  }

  for (const FenceAction& fence : state.fences) {
    builder.out.append("fence ");
    builder.out.append(to_string(fence.kind));
    builder.out.push_back(' ');
    builder.out.append(fence.subject.empty() ? "-" : fence.subject);
    builder.out.push_back(' ');
    builder.out.append(to_string(fence.reason.code));
    if (!fence.reason.detail.empty()) {
      builder.out.push_back(' ');
      builder.out.append(escape_text(fence.reason.detail));
    }
    builder.out.push_back('\n');
  }

  for (const Reason& reason : state.degradations) {
    builder.out.append("degradation ");
    builder.out.append(to_string(reason.code));
    if (!reason.detail.empty()) {
      builder.out.push_back(' ');
      builder.out.append(escape_text(reason.detail));
    }
    builder.out.push_back('\n');
  }

  if (include_decisions) {
    for (const DecisionRecord& decision : state.decisions) {
      builder.out.append("decision ");
    builder.out.append(decision.id.token());
    builder.out.append(" kind ");
    builder.out.append(to_string(decision.kind));
    builder.out.append(" status ");
    builder.out.append(to_string(decision.status));
    builder.out.append(" deps ");
    builder.out.append(std::to_string(decision.deps.size()));
      builder.out.append(" fingerprint ");
      builder.out.append(decision.fingerprint.to_hex());
      builder.out.push_back('\n');
      for (const DepKey& dep : decision.deps) {
        builder.out.append("+dep ");
        builder.out.append(to_string(dep.kind));
        builder.out.push_back(' ');
        builder.out.append(dep.subject);
        builder.out.push_back('\n');
      }
      for (const Reason& reason : decision.reasons) {
        builder.out.append("+reason ");
        builder.out.append(to_string(reason.code));
        if (!reason.detail.empty()) {
          builder.out.push_back(' ');
          builder.out.append(escape_text(reason.detail));
        }
        builder.out.push_back('\n');
      }
    }
  }

  builder.out.append("composed-at ");
  builder.out.append(std::to_string(state.composed_at));
  builder.push_back_newline();
  builder.out.append("digest ");
  builder.out.append(state.state_digest.to_hex());
  builder.out.push_back('\n');
  builder.out.append("decision-digest ");
  builder.out.append(state.decision_digest.to_hex());
  builder.out.push_back('\n');
  builder.line("end");
  return builder.out;
}

}  // namespace podfabric::codec
