#include "common.h"
#include "rewriter-server.h"
#include <IO/Operators.h>
#include <Parsers/ASTAlterQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTQueryParameter.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTUpdateQuery.h>
#include <Parsers/ASTUseQuery.h>
#include <Parsers/ASTWithElement.h>
#include <Parsers/CommonParsers.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/Lexer.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/ParserWithElement.h>
#include <Parsers/parseQuery.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace snapshot_query {
namespace {
using DB::ASTPtr;
using namespace rewriter;
using Object = Poco::JSON::Object::Ptr;
struct Refusal {
  SnapshotQueryCode code;
  std::string message;
};
[[noreturn]] void refuse(const char *message, SnapshotQueryCode code = UNSUPPORTED) {
  throw Refusal{code, std::string("snapshot query ") + message};
}
[[noreturn]] void syntax() {
  refuse("syntax is outside the closed profile");
}
[[noreturn]] void binding() {
  refuse("column or relation cannot be bound in its lexical scope");
}
[[noreturn]] void literal() {
  refuse("literal form is outside the exact integer profile");
}
[[noreturn]] void range() {
  refuse("integer literal is outside the exact target range", INVALID_INPUT);
}
[[noreturn]] void target() {
  refuse("target columns do not match the authenticated schema", INVALID_INPUT);
}
[[noreturn]] void scalar() {
  refuse("scalar output is outside the exact integer profile");
}
std::string lower(std::string s) {
  for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool nameValid(const std::string &s) {
  return !s.empty() && s.find('\0') == std::string::npos;
}
void identifier(const std::string &s) {
  if (!nameValid(s)) syntax();
  if (s == "_hg_row_id") refuse("cannot reference the reserved row id");
}
bool digest(const std::string &s) {
  return s.size() == 66 && s.starts_with("0x") && std::all_of(s.begin() + 2, s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
struct Limits {
  uint64_t sql = 0, descriptor = 0, ms = 0;
};
bool supported(const std::string *raw, Limits &limits) {
  if (!raw) return false;
  try {
    Poco::JSON::Parser parser;
    auto o = parser.parse(*raw).extract<Object>();
    if (o->getValue<int>("version") != 1 ||
        o->getValue<std::string>("column_profile_id") != "column-profile-v1" ||
        o->getValue<std::string>("output_order_id") != "output-order-v1")
      return false;
    const std::vector<std::pair<std::string, std::string>> settings = {{"cast_keep_nullable", "0"},
      {"max_threads", "1"},
      {"read_overflow_mode", "throw"},
      {"result_overflow_mode", "throw"},
      {"sort_overflow_mode", "throw"},
      {"timeout_overflow_mode", "throw"}};
    auto s = o->getArray("settings");
    if (!s || s->size() != settings.size()) return false;
    for (size_t i = 0; i < s->size(); ++i)
      if (s->getObject(i)->getValue<std::string>("name") != settings[i].first ||
          s->getObject(i)->getValue<std::string>("value") != settings[i].second)
        return false;
    const std::vector<std::string> ops = {"and",
      "column",
      "equals",
      "greater",
      "greaterOrEquals",
      "in",
      "less",
      "lessOrEquals",
      "literal",
      "not",
      "notEquals",
      "or",
      "prepare-integer-literal-exact-v1",
      "prepare-integer-scalar-null-throw-v1",
      "rand",
      "rand32",
      "rand64"};
    auto a = o->getArray("scalar_operators");
    if (!a || a->size() != ops.size()) return false;
    for (size_t i = 0; i < a->size(); ++i)
      if (a->getElement<std::string>(i) != ops[i]) return false;
    auto l = o->getObject("limits");
    limits = {l->getValue<uint64_t>("max_sql_bytes"),
      l->getValue<uint64_t>("max_descriptor_bytes"),
      l->getValue<uint64_t>("max_execution_ms")};
    return limits.sql > 0 && limits.sql <= 65536 && limits.descriptor > 0 &&
           limits.descriptor <= 65536 && limits.ms > 0 && limits.ms <= 30000;
  } catch (...) { return false; }
}
void check(const Execution &e, const Limits &l) {
  if ((e.cancelled && e.cancelled()) ||
      std::chrono::steady_clock::now() - e.started >= std::chrono::milliseconds(l.ms))
    refuse("execution deadline exceeded", INVALID_INPUT);
}
// A generation-only adapter for the already validated closed boolean grammar.
// ClickHouse's generic function formatter brackets every comparison operand;
// the frozen contract uses explicit mixed-logical groups without those brackets.
class CanonicalBoolean final : public DB::ASTFunction {
public:
  explicit CanonicalBoolean(const DB::ASTFunction &function) : DB::ASTFunction(function) {}
  static int precedence(const DB::IAST *node) {
    // ClickHouse TypePromotion::as requires exact dynamic type; generated
    // subclasses must retain their operator precedence as well.
    const auto *f = dynamic_cast<const DB::ASTFunction *>(node);
    if (!f) return 5;
    if (f->name == "or") return 1;
    if (f->name == "and") return 2;
    if (f->name == "not") return 3;
    for (auto name :
      {"equals", "notEquals", "less", "lessOrEquals", "greater", "greaterOrEquals", "in"})
      if (f->name == name) return 4;
    return 5;
  }
  DB::ASTPtr clone() const override {
    auto copy = DB::ASTFunction::clone();
    return DB::makeRewriterAST<CanonicalBoolean>(*copy->as<DB::ASTFunction>());
  }

protected:
  void formatImplWithoutAlias(DB::WriteBuffer &out,
    const FormatSettings &settings,
    FormatState &state,
    FormatStateStacked frame) const override {
    const int parent = precedence(this);
    if (frame.need_parens) out << '(';
    auto operand = [&](const ASTPtr &child) {
      auto nested = frame;
      const int rank = precedence(child.get());
      // Preserve mixed logical grouping explicitly. Comparisons are not
      // associative; either nested comparison operand must remain grouped.
      nested.need_parens = (parent <= 2 ? rank <= 2 && rank != parent : rank <= 4);
      child->format(out, settings, state, nested);
    };
    if (name == "not") {
      out << "NOT ";
      operand(arguments->children.at(0));
    } else {
      const std::map<std::string, std::string> operators = {{"or", " OR "},
        {"and", " AND "},
        {"equals", " = "},
        {"notEquals", " != "},
        {"less", " < "},
        {"lessOrEquals", " <= "},
        {"greater", " > "},
        {"greaterOrEquals", " >= "},
        {"in", " IN "}};
      const auto &op = operators.at(name);
      for (size_t i = 0; i < arguments->children.size(); ++i) {
        if (i) out << op;
        operand(arguments->children[i]);
      }
    }
    if (frame.need_parens) out << ')';
  }
};
void canonicalBooleans(ASTPtr &node) {
  // Only a clone is changed. All retained binding/type/closure ASTs are intact.
  for (auto &child : node->children) {
    auto old = child;
    canonicalBooleans(child);
    if (old != child) node->updatePointerToChild(old.get(), child);
  }
  if (auto *f = node->as<DB::ASTFunction>(); f && CanonicalBoolean::precedence(f) < 5)
    node = DB::makeRewriterAST<CanonicalBoolean>(*f);
}
std::string generate(const ASTPtr &ast, Execution &e, const Limits &l) {
  check(e, l);
  auto output = ast->clone();
  canonicalBooleans(output);
  auto sql = rewriter_handlers::formatAst(output);
  if (e.after_generation) e.after_generation();
  check(e, l);
  return sql;
}
int integerBits(const std::string &t) {
  for (auto n : {8, 16, 32, 64})
    if (t == "Int" + std::to_string(n) || t == "UInt" + std::to_string(n)) return n;
  return 0;
}
bool timezone(const std::string &s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isalnum(c) || std::string("/_+-").find(c) != std::string::npos;
  });
}
bool columnType(const std::string &t) {
  if (integerBits(t) || t == "String" || t == "Bool" || t == "Float32" || t == "Float64" ||
      t == "Date" || t == "DateTime" || t == "FixedString(32)")
    return true;
  if (t.starts_with("DateTime('") && t.ends_with("')"))
    return timezone(t.substr(10, t.size() - 12));
  if (!t.starts_with("DateTime64(") || !t.ends_with(')')) return false;
  auto s = t.substr(11, t.size() - 12);
  if (s.empty() || s[0] < '0' || s[0] > '9') return false;
  if (s.size() == 1) return true;
  return s.size() >= 6 && s.substr(1, 3) == ", '" && s.back() == '\'' &&
         timezone(s.substr(4, s.size() - 5));
}
// Decimal mathematics uses bounded sign/digit strings. No floating conversion
// participates in proof, including before the parser's Float64 normalization.
std::string decimal(std::string s) {
  if (s.empty() || s.size() > 256 ||
      !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
    literal();
  auto first = s.find_first_not_of('0');
  return first == std::string::npos ? "0" : s.substr(first);
}
bool fits(const std::string &n, const std::string &type) {
  int bits = integerBits(type);
  if (!bits) return false;
  bool neg = n.starts_with('-'), sign = type.starts_with("Int");
  auto v = neg ? n.substr(1) : n;
  if (neg && !sign) return false;
  uint64_t max =
    sign ? (uint64_t(1) << (bits - 1)) : (bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1);
  if (sign && !neg) --max;
  auto bound = std::to_string(max);
  return v.size() < bound.size() || (v.size() == bound.size() && v <= bound);
}
std::string smallest(const std::string &n) {
  for (int b : {8, 16, 32, 64}) {
    auto t = (n.starts_with('-') ? "Int" : "UInt") + std::to_string(b);
    if (fits(n, t)) return t;
  }
  range();
}
ASTPtr stringNode(const std::string &s) {
  return DB::makeRewriterAST<DB::ASTLiteral>(DB::Field(s));
}
void eligible(const SnapshotQueryCatalogTable &t, bool types) {
  std::set<std::string> names;
  for (const auto &c : t.columns()) {
    if (c.generation() != SNAPSHOT_QUERY_COLUMN_GENERATION_ORDINARY ||
        !c.default_expression().empty())
      refuse("requires ordinary columns with empty generation expressions");
    identifier(c.name());
    if (!names.insert(c.name()).second) target();
    if (types && !columnType(c.type())) refuse("type is outside the column profile");
  }
}
struct Value {
  std::string type, name, number;
  bool nullable = false, scalar = false, string_literal = false;
  ASTPtr node;
};
struct Bound {
  const SnapshotQueryCatalogTable *table;
  ASTPtr node;
  std::string qualifier;
  bool qualified = false;
  std::set<std::string> other;
};
struct Definition;
struct Query {
  std::vector<Value> columns;
  std::set<std::string> reads;
  std::set<std::shared_ptr<Bound>> bases;
  std::set<std::shared_ptr<Definition>> used;
  std::vector<DB::ASTSelectQuery *> leaves;
};
struct Definition {
  ASTPtr node;
  Query query;
};
using Scope = std::map<std::string, std::shared_ptr<Definition>>;
struct Relation {
  Query query;
  std::string qualifier;
  std::shared_ptr<Bound> bound;
};
struct With {
  ASTPtr list;
  DB::ASTSelectQuery *owner;
  std::vector<std::shared_ptr<Definition>> definitions;
};
void merge(Query &q, const Query &r) {
  q.reads.insert(r.reads.begin(), r.reads.end());
  q.bases.insert(r.bases.begin(), r.bases.end());
  q.used.insert(r.used.begin(), r.used.end());
}
std::vector<Value> derived(std::vector<Value> v) {
  for (auto &x : v) {
    x.number.clear();
    x.scalar = false;
  }
  return v;
}
std::vector<DB::Token> tokens(const std::string &sql) {
  DB::Lexer lexer(sql.data(), sql.data() + sql.size());
  std::vector<DB::Token> out;
  int depth = 0;
  bool ended = false;
  for (auto t = lexer.nextToken(); !t.isEnd(); t = lexer.nextToken()) {
    if (t.isError()) refuse("requires one recognized statement", INVALID_INPUT);
    if (!t.isSignificant()) continue;
    if (ended) refuse("requires one recognized statement", INVALID_INPUT);
    if (t.type == DB::TokenType::OpeningRoundBracket ||
        t.type == DB::TokenType::OpeningSquareBracket) {
      if (++depth > 128) syntax();
    }
    if (t.type == DB::TokenType::ClosingRoundBracket ||
        t.type == DB::TokenType::ClosingSquareBracket) {
      if (--depth < 0) refuse("requires one recognized statement", INVALID_INPUT);
    }
    if (t.type == DB::TokenType::Semicolon) {
      if (depth) refuse("requires one recognized statement", INVALID_INPUT);
      ended = true;
    }
    out.push_back(t);
  }
  if (depth) refuse("requires one recognized statement", INVALID_INPUT);
  return out;
}
ASTPtr parse(const std::string &sql) {
  DB::ParserQuery parser(sql.data() + sql.size());
  return DB::parseQuery(parser, sql.data(), sql.data() + sql.size(), "", 65536, 128, 100000);
}
struct Analyzer {
  const AnalyzeSnapshotQueryRequest &request;
  Execution &execution;
  const Limits &limits;
  ASTPtr root, outer;
  std::string outer_text, body_text;
  std::map<std::pair<std::string, std::string>, const SnapshotQueryCatalogTable *> catalog;
  std::map<std::string, const SnapshotQueryCatalogTable *> ids;
  std::map<const DB::IAST *, std::string> numbers;
  std::set<std::string> all_ctes;
  std::vector<With> withs;
  const SnapshotQueryCatalogTable *destination = nullptr;
  std::vector<std::string> target_names;
  std::vector<size_t> order;
  Query result;
  bool ordinary = false;
  size_t pool = 0, nodes = 0;
  Analyzer(const AnalyzeSnapshotQueryRequest &r, Execution &e, const Limits &l)
      : request(r), execution(e), limits(l) {}
  void bounded(const ASTPtr &n, size_t depth = 0) {
    if (!n) return;
    if (depth > 128 || ++nodes > 16384) syntax();
    check(execution, limits);
    for (auto &c : n->children) bounded(c, depth + 1);
  }
  void parseInput() {
    auto ts = tokens(request.sql());
    if (ts.empty()) refuse("requires one recognized statement", INVALID_INPUT);
    size_t split = std::string::npos;
    // Follow the vendor INSERT parser's WITH-definition grammar to the actual
    // statement-control position. A BareWord INSERT may itself be a CTE name
    // or an ordinary SELECT column, so searching top-level words is ambiguous.
    DB::Tokens stream(request.sql().data(), request.sql().data() + request.sql().size(), 65536);
    DB::IParser::Pos position(stream, 128, 100000);
    DB::Expected expected;
    if (DB::ParserKeyword(DB::Keyword::WITH).ignore(position, expected)) {
      ASTPtr definitions;
      DB::ParserList definition_list(std::make_unique<DB::ParserWithElement>(),
        std::make_unique<DB::ParserToken>(DB::TokenType::Comma));
      if (definition_list.parse(position, definitions, expected) && definitions &&
          !definitions->children.empty()) {
        const char *boundary = position->begin;
        if (DB::ParserKeyword(DB::Keyword::INSERT_INTO).ignore(position, expected))
          split = boundary - request.sql().data();
      }
    }
    body_text = request.sql();
    try {
      if (split != std::string::npos) {
        outer_text = request.sql().substr(0, split);
        body_text = request.sql().substr(split);
        auto prefix = parse(outer_text + " SELECT 1");
        auto *u = prefix->as<DB::ASTSelectWithUnionQuery>();
        if (!u || !u->list_of_selects || u->list_of_selects->children.size() != 1) syntax();
        auto *s = u->list_of_selects->children[0]->as<DB::ASTSelectQuery>();
        if (!s || s->recursive_with || !s->with()) syntax();
        outer = s->with();
      }
      root = parse(body_text);
    } catch (const Refusal &) { throw; } catch (...) {
      refuse("requires one recognized statement", INVALID_INPUT);
    }
    bounded(outer);
    bounded(root);
  }
  void rejectFoldedCarriers(const ASTPtr &n) {
    if (!n) return;
    // These grammar nodes can consume or fold numeric tokens into non-literal
    // fields. Refuse their syntax before attempting the numeric leaf bijection.
    if (auto *t = n->as<DB::ASTTableExpression>(); t && (t->sample_size || t->sample_offset))
      syntax();
    if (auto *t = n->as<DB::ASTTablesInSelectQueryElement>(); t && t->array_join) syntax();
    if (auto *s = n->as<DB::ASTSelectQuery>(); s && s->settings()) syntax();
    if (auto *u = n->as<DB::ASTSelectWithUnionQuery>(); u && u->settings_ast) syntax();
    if (auto *f = n->as<DB::ASTFunction>();
      f && (lower(f->name) == "cast" || lower(f->name) == "_cast"))
      syntax();
    for (auto &c : n->children) rejectFoldedCarriers(c);
  }
  void bindNumbers(const ASTPtr &ast, const std::string &sql) {
    auto ts = tokens(sql);
    std::vector<std::string> lex;
    for (size_t i = 0; i < ts.size(); ++i) {
      auto &t = ts[i];
      if (t.type == DB::TokenType::OpeningCurlyBrace || t.type == DB::TokenType::QuestionMark ||
          t.type == DB::TokenType::DollarSign)
        refuse("parameters are not supported");
      if (t.type == DB::TokenType::Plus) {
        size_t j = i + 1;
        while (j < ts.size() && ts[j].type == DB::TokenType::OpeningRoundBracket) ++j;
        if (j == ts.size() || ts[j].type != DB::TokenType::Number) syntax();
      }
      if (t.type != DB::TokenType::Number) continue;
      size_t signs = 0;
      for (size_t j = i; j > 0;) {
        auto type = ts[--j].type;
        if (type == DB::TokenType::Plus || type == DB::TokenType::Minus)
          ++signs;
        else if (type != DB::TokenType::OpeningRoundBracket)
          break;
      }
      if (signs > 1) literal();
      lex.emplace_back(t.begin, t.end);
    }
    size_t next = 0;
    std::function<void(const ASTPtr &)> walk = [&](const ASTPtr &n) {
      if (!n) return;
      if (auto *l = n->as<DB::ASTLiteral>();
        l && (l->value.getType() == DB::Field::Types::UInt64 ||
               l->value.getType() == DB::Field::Types::Int64 ||
               l->value.getType() == DB::Field::Types::Float64)) {
        if (next >= lex.size()) literal();
        numbers[n.get()] = lex[next++];
        return;
      }
      for (auto &c : n->children) walk(c);
    };
    walk(ast);
    if (next != lex.size()) literal();
  }
  bool volatileName(const std::string &s) {
    static const std::set<std::string> names = {"rand",
      "random",
      "rand32",
      "rand64",
      "randcanonical",
      "randconstant",
      "randomstring",
      "now",
      "now64",
      "today",
      "yesterday",
      "current_timestamp",
      "currenttimestamp",
      "current_date",
      "currentdate",
      "localtime",
      "localtimestamp",
      "utc_timestamp",
      "generateuuidv4",
      "generateuuidv7",
      "nowinblock",
      "nowinblock64"};
    return names.contains(lower(s));
  }
  void materialize(ASTPtr &n) {
    if (!n) return;
    if (auto *f = n->as<DB::ASTFunction>(); f && volatileName(f->name)) {
      auto name = lower(f->name);
      if ((name != "rand" && name != "rand32" && name != "rand64") || !f->arguments ||
          !f->arguments->children.empty() || f->parameters || f->isWindowFunction())
        refuse("volatile function is outside the closed profile");
      if (!request.materialize()) refuse("contains residual volatility", MATERIALIZATION_FAILED);
      if (!request.has_inputs() ||
          pool >= static_cast<size_t>(request.inputs().random_uint64_values_size()))
        refuse("materialization is incomplete or unsupported", MATERIALIZATION_FAILED);
      auto v = request.inputs().random_uint64_values(pool++);
      if (name != "rand64" && v > UINT32_MAX)
        refuse("materialization is incomplete or unsupported", MATERIALIZATION_FAILED);
      auto replacement = DB::makeRewriterAST<DB::ASTLiteral>(DB::Field(static_cast<UInt64>(v)));
      replacement->setAlias(n->tryGetAlias());
      numbers[replacement.get()] = std::to_string(v);
      n = replacement;
      return;
    }
    // Mutation must update member pointers as well as the owning children list.
    for (auto &child : n->children) {
      auto old = child;
      materialize(child);
      if (old == child) continue;
      if (auto *f = n->as<DB::ASTFunction>()) {
        if (f->arguments == old) f->arguments = child;
        if (f->parameters == old) f->parameters = child;
      }
    }
  }
  const SnapshotQueryCatalogTable *base(std::string db, const std::string &name) {
    identifier(name);
    if (db.empty()) db = request.logical_database();
    identifier(db);
    auto it = catalog.find({db, name});
    if (it == catalog.end() || db == "system" || db == "information_schema" ||
        db == "INFORMATION_SCHEMA" || db == "hg_safe" || db == "hg_unsafe")
      refuse("relation is absent from the authenticated catalog");
    return it->second;
  }
  Scope with(const ASTPtr &list, Scope scope, DB::ASTSelectQuery *owner = nullptr) {
    if (!list) return scope;
    if (!list->as<DB::ASTExpressionList>()) syntax();
    for (auto &n : list->children) {
      auto *w = n->as<DB::ASTWithElement>();
      if (!w) syntax();
      identifier(w->name);
      all_ctes.insert(w->name);
    }
    With record{list, owner, {}};
    std::set<std::string> local;
    for (auto &n : list->children) {
      auto *w = n->as<DB::ASTWithElement>();
      if (w->aliases || w->is_materialized || !w->subquery || w->children.size() != 1) syntax();
      if (!local.insert(w->name).second) binding();
      auto *sub = w->subquery->as<DB::ASTSubquery>();
      if (!sub || sub->children.size() != 1) syntax();
      auto d = std::make_shared<Definition>(Definition{n, query(sub->children[0], scope)});
      record.definitions.push_back(d);
      scope[w->name] = d;
    }
    withs.push_back(std::move(record));
    return scope;
  }
  Relation relation(const ASTPtr &node, const Scope &scope) {
    auto *t = node->as<DB::ASTTableExpression>();
    if (!t || t->final || t->sample_size || t->sample_offset || t->column_aliases ||
        t->table_function || t->children.size() != 1)
      syntax();
    Relation r;
    if (t->database_and_table_name) {
      auto *id = t->database_and_table_name->as<DB::ASTTableIdentifier>();
      if (!id || id->isParam()) refuse("parameters are not supported");
      if (id->name_parts.empty() || id->name_parts.size() > 2) syntax();
      auto name = id->shortName();
      identifier(name);
      r.qualifier = id->tryGetAlias();
      if (r.qualifier.empty()) r.qualifier = name;
      identifier(r.qualifier);
      if (id->name_parts.size() == 1) {
        auto it = scope.find(name);
        if (it != scope.end()) {
          r.query = it->second->query;
          r.query.columns = derived(r.query.columns);
          r.query.used.insert(it->second);
          return r;
        }
        if (all_ctes.contains(name) && !catalog.contains({request.logical_database(), name}))
          binding();
      }
      auto *b = base(id->name_parts.size() == 2 ? id->name_parts[0] : "", name);
      r.query.reads.insert(b->table_id());
      r.bound =
        std::make_shared<Bound>(Bound{b, t->database_and_table_name, r.qualifier, false, {}});
      r.query.bases.insert(r.bound);
      for (auto &c : b->columns())
        r.query.columns.push_back(Value{c.type(), c.name(), "", false, false, false, {}});
      return r;
    }
    auto *s = t->subquery ? t->subquery->as<DB::ASTSubquery>() : nullptr;
    if (!s || s->children.size() != 1) syntax();
    r.query = query(s->children[0], scope);
    r.query.columns = derived(r.query.columns);
    r.qualifier = s->tryGetAlias();
    if (!r.qualifier.empty()) identifier(r.qualifier);
    return r;
  }
  bool onlyLiterals(const ASTPtr &n) {
    if (n->as<DB::ASTLiteral>()) return true;
    auto *f = n->as<DB::ASTFunction>();
    if (!f || !f->arguments || f->parameters) return false;
    const std::set<std::string> arithmetic = {
      "plus", "minus", "multiply", "divide", "modulo", "negate"};
    return arithmetic.contains(f->name) && std::all_of(f->arguments->children.begin(),
                                             f->arguments->children.end(),
                                             [&](auto &c) { return onlyLiterals(c); });
  }
  Value expression(const ASTPtr &n, std::vector<Relation> &rels, const Scope &scope, Query &q) {
    Value v;
    v.node = n;
    if (!n) syntax();
    if (n->as<DB::ASTQueryParameter>()) refuse("parameters are not supported");
    if (auto *id = n->as<DB::ASTIdentifier>()) {
      if (id->isParam()) refuse("parameters are not supported");
      if (id->name_parts.empty() || id->name_parts.size() > 2) binding();
      auto name = id->shortName();
      identifier(name);
      std::string qual = id->name_parts.size() == 2 ? id->name_parts[0] : "";
      size_t found = 0;
      for (auto &r : rels) {
        if (!qual.empty() && qual != r.qualifier) continue;
        for (auto &c : r.query.columns)
          if (c.name == name) {
            v = c;
            v.node = n;
            v.number.clear();
            v.scalar = false;
            ++found;
            if (!qual.empty() && r.bound) r.bound->qualified = true;
          }
      }
      if (found != 1) {
        if (name.starts_with("0b")) literal();
        binding();
      }
    } else if (auto *l = n->as<DB::ASTLiteral>()) {
      if (l->value.getType() == DB::Field::Types::String) {
        v.type = "String";
        v.string_literal = true;
      } else {
        auto it = numbers.find(n.get());
        if (it == numbers.end()) literal();
        v.number = decimal(it->second);
        if (l->value.getType() == DB::Field::Types::Int64 && l->value.safeGet<Int64>() < 0 &&
            v.number != "0")
          v.number = "-" + v.number;
        // A normalized floating result is never used to recover the original token.
        // Its raw decimal magnitude is checked before the representation refusal.
        if (!fits(v.number, v.number.starts_with('-') ? "Int64" : "UInt64")) range();
        if (l->value.getType() == DB::Field::Types::Float64) range();
        auto parsed = l->value.getType() == DB::Field::Types::Int64
                        ? std::to_string(l->value.safeGet<Int64>())
                        : std::to_string(l->value.safeGet<UInt64>());
        if (parsed != v.number) literal();
        v.type = smallest(v.number);
      }
    } else if (auto *s = n->as<DB::ASTSubquery>()) {
      if (s->children.size() != 1) syntax();
      auto inner = query(s->children[0], scope);
      if (inner.columns.size() != 1) scalar();
      merge(q, inner);
      v = inner.columns[0];
      v.node = n;
      v.name.clear();
      v.scalar = true;
      v.nullable = true;
    } else if (auto *f = n->as<DB::ASTFunction>()) {
      if (!f->arguments || f->parameters || f->isWindowFunction() || f->isLambdaFunction() ||
          f->window_definition || !f->window_name.empty() || f->children.size() != 1)
        syntax();
      auto &args = f->arguments->children;
      if (f->name == "negate" && args.size() == 1) {
        if (!args[0]->as<DB::ASTLiteral>()) {
          if (onlyLiterals(args[0])) literal();
          syntax();
        }
        v = expression(args[0], rels, scope, q);
        if (v.number.empty() || v.number.starts_with('-')) literal();
        if (v.number != "0") v.number = "-" + v.number;
        if (!fits(v.number, "Int64")) range();
        v.type = smallest(v.number);
        v.node = n;
      } else if (f->name == "in" && args.size() == 2) {
        auto left = expression(args[0], rels, scope, q);
        auto *sub = args[1]->as<DB::ASTSubquery>();
        if (!sub || sub->children.size() != 1) syntax();
        auto inner = query(sub->children[0], scope);
        if (inner.columns.size() != 1 || !integerBits(left.type) ||
            !integerBits(inner.columns[0].type))
          syntax();
        merge(q, inner);
        v.type = "UInt8";
      } else {
        static const std::set<std::string> binary = {
          "equals", "notEquals", "greater", "greaterOrEquals", "less", "lessOrEquals", "and", "or"};
        if (!((binary.contains(f->name) && args.size() >= 2 &&
                (args.size() == 2 || f->name == "and" || f->name == "or")) ||
              (f->name == "not" && args.size() == 1))) {
          if (onlyLiterals(n)) literal();
          syntax();
        }
        for (auto &arg : args) {
          Value x;
          try {
            x = expression(arg, rels, scope, q);
          } catch (const Refusal &r) {
            if (r.message == "snapshot query literal form is outside the exact integer profile")
              syntax();
            throw;
          }
          if (!integerBits(x.type)) syntax();
          v.nullable |= x.nullable;
        }
        v.type = "UInt8";
      }
    } else
      syntax();
    auto alias = n->tryGetAlias();
    if (!alias.empty()) {
      identifier(alias);
      for (auto &r : rels)
        for (auto &c : r.query.columns)
          if (c.name == alias) {
            auto *id = n->as<DB::ASTIdentifier>();
            if (!id || id->shortName() != alias ||
                (id->name_parts.size() == 2 && id->name_parts[0] != r.qualifier))
              binding();
          }
      v.name = alias;
    }
    return v;
  }
  Query query(const ASTPtr &node, Scope scope, bool skip_with = false) {
    check(execution, limits);
    Query q;
    if (auto *u = node->as<DB::ASTSelectWithUnionQuery>()) {
      if (!u->list_of_selects || u->hasOutputOptions() || u->settings_ast ||
          u->children.size() != 1 || u->list_of_selects->children.empty())
        syntax();
      for (auto mode : u->list_of_modes)
        if (mode != DB::SelectUnionMode::UNION_ALL) syntax();
      auto *first = u->list_of_selects->children.front()->as<DB::ASTSelectQuery>();
      bool shared_with = u->list_of_selects->children.size() > 1 && first && first->with();
      if (shared_with) {
        if (first->recursive_with) syntax();
        scope = with(first->with(), scope, first);
      }
      for (auto &s : u->list_of_selects->children) {
        auto part = query(s, scope, shared_with && s.get() == first);
        if (q.leaves.empty())
          q.columns = part.columns;
        else {
          if (q.columns.size() != part.columns.size()) target();
          for (size_t i = 0; i < q.columns.size(); ++i) {
            if (q.columns[i].nullable || part.columns[i].nullable) scalar();
            if (q.columns[i].type != part.columns[i].type)
              refuse("output type does not exactly match the target");
          }
        }
        merge(q, part);
        q.leaves.insert(q.leaves.end(), part.leaves.begin(), part.leaves.end());
      }
      if (q.leaves.size() > 1) q.columns = derived(q.columns);
      return q;
    }
    auto *s = node->as<DB::ASTSelectQuery>();
    if (!s || s->recursive_with || s->distinct || s->group_by_all || s->group_by_with_totals ||
        s->group_by_with_rollup || s->group_by_with_cube || s->group_by_with_constant_keys ||
        s->group_by_with_grouping_sets || s->order_by_all || s->limit_with_ties || s->limit_by_all)
      syntax();
    for (auto &child : s->children)
      if (child != s->with() && child != s->select() && child != s->tables() && child != s->where())
        syntax();
    if (!skip_with) scope = with(s->with(), scope, s);
    std::vector<Relation> rels;
    if (auto table_list = s->tables()) {
      if (!table_list->as<DB::ASTTablesInSelectQuery>()) syntax();
      for (auto &child : table_list->children) {
        auto *el = child->as<DB::ASTTablesInSelectQueryElement>();
        if (!el || el->array_join || !el->table_expression) syntax();
        if (rels.empty()) {
          if (el->table_join || el->children.size() != 1) syntax();
        } else {
          auto *j = el->table_join ? el->table_join->as<DB::ASTTableJoin>() : nullptr;
          if (!j || j->kind != DB::JoinKind::Inner || j->strictness != DB::JoinStrictness::All ||
              j->locality != DB::JoinLocality::Unspecified || j->using_expression_list ||
              !j->on_expression || j->children.size() != 1 || el->children.size() != 2)
            syntax();
          auto *on = j->on_expression->as<DB::ASTFunction>();
          if (!on || on->name != "equals") syntax();
        }
        rels.push_back(relation(el->table_expression, scope));
        if (el->table_join)
          expression(el->table_join->as<DB::ASTTableJoin>()->on_expression, rels, scope, q);
      }
    }
    std::set<std::string> quals;
    for (auto &r : rels) {
      if (!r.qualifier.empty() && !quals.insert(r.qualifier).second) binding();
      merge(q, r.query);
    }
    for (auto &r : rels)
      if (r.bound)
        for (auto &other : rels)
          if (other.qualifier != r.qualifier) r.bound->other.insert(other.qualifier);
    auto expressions = s->select();
    if (!expressions || !expressions->as<DB::ASTExpressionList>() || expressions->children.empty())
      syntax();
    for (auto &n : expressions->children) q.columns.push_back(expression(n, rels, scope, q));
    if (auto where = s->where()) {
      auto v = expression(where, rels, scope, q);
      if (!integerBits(v.type) && v.type != "Bool") syntax();
    }
    q.leaves.push_back(s);
    return q;
  }
  void output(const Value &v, const std::string &t, bool direct) {
    if (!v.number.empty() && !v.nullable && direct) {
      if (!columnType(t)) refuse("type is outside the column profile");
      if (!integerBits(t)) refuse("output type does not exactly match the target");
      if (!fits(v.number, t)) range();
      return;
    }
    if (v.nullable) {
      if (!v.scalar || !direct) scalar();
      if (v.type != t) {
        if (!v.number.empty()) refuse("output type does not exactly match the target");
        scalar();
      }
      if (!integerBits(t)) scalar();
      return;
    }
    if (v.type != t) {
      if (v.string_literal && integerBits(t)) literal();
      refuse("output type does not exactly match the target");
    }
  }
  void analyze() {
    parseInput();
    auto *ins = root->as<DB::ASTInsertQuery>();
    if (!ins) {
      if (!outer && (root->as<DB::ASTSelectWithUnionQuery>() || root->as<DB::ASTUseQuery>() ||
                      root->as<DB::ASTCreateQuery>() || root->as<DB::ASTDropQuery>() ||
                      root->as<DB::ASTAlterQuery>() || root->as<DB::ASTUpdateQuery>() ||
                      root->as<DB::ASTDeleteQuery>())) {
        ordinary = true;
        return;
      }
      refuse("requires one recognized statement", INVALID_INPUT);
    }
    if (!ins->select && ins->format == "Values" && ins->data && !outer) {
      try {
        DB::ParserExpressionList values(false);
        auto value_ast = DB::parseQuery(values, ins->data, ins->end, "", 65536, 128, 100000);
        if (!value_ast || value_ast->children.empty())
          refuse("requires one recognized statement", INVALID_INPUT);
      } catch (...) { refuse("requires one recognized statement", INVALID_INPUT); }
      ordinary = true;
      return;
    }
    if (!ins->select || ins->table_function || ins->partition_by || ins->infile ||
        ins->compression || ins->settings_ast || !ins->format.empty() || ins->hasInlinedData() ||
        ins->async_insert_flush)
      syntax();
    for (auto &t : request.catalog()) {
      if (!nameValid(t.database()) || !nameValid(t.table()) ||
          !catalog.emplace(std::make_pair(t.database(), t.table()), &t).second ||
          !ids.emplace(t.table_id(), &t).second)
        refuse("relation is absent from the authenticated catalog");
    }
    rejectFoldedCarriers(outer);
    rejectFoldedCarriers(root);
    if (outer) bindNumbers(outer, outer_text);
    bindNumbers(root, body_text);
    materialize(outer);
    materialize(root);
    destination = base(ins->getDatabase(), ins->getTable());
    if (destination->columns().empty()) target();
    eligible(*destination, false);
    if (ins->columns)
      for (auto &c : ins->columns->children) {
        auto *id = c->as<DB::ASTIdentifier>();
        if (!id || id->isParam() || id->name_parts.size() != 1) syntax();
        identifier(id->name());
        target_names.push_back(id->name());
      }
    else
      for (auto &c : destination->columns()) target_names.push_back(c.name());
    if (target_names.size() != static_cast<size_t>(destination->columns_size())) target();
    std::set<std::string> seen;
    for (auto &c : destination->columns()) {
      auto it = std::find(target_names.begin(), target_names.end(), c.name());
      if (it == target_names.end() || !seen.insert(c.name()).second ||
          std::count(target_names.begin(), target_names.end(), c.name()) != 1)
        target();
      order.push_back(it - target_names.begin());
    }
    auto scope = with(outer, {});
    result = query(ins->select, scope);
    if (result.columns.size() != target_names.size()) target();
    for (auto &c : destination->columns())
      if (!columnType(c.type()) && !c.type().starts_with("FixedString(") &&
          !c.type().starts_with("DateTime(") && !c.type().starts_with("DateTime64("))
        refuse("type is outside the column profile");
    for (size_t i = 0; i < order.size(); ++i)
      output(result.columns[order[i]], destination->columns(i).type(), result.leaves.size() == 1);
    eligible(*destination, true);
    for (auto &id : result.reads) eligible(*ids.at(id), true);
    if (pool != static_cast<size_t>(request.inputs().random_uint64_values_size()))
      refuse("materialization is incomplete or unsupported", MATERIALIZATION_FAILED);
  }
  std::string logical() {
    auto sql = generate(root, execution, limits);
    if (outer) sql = "WITH " + generate(outer, execution, limits) + " " + sql;
    return sql;
  }
  std::string prepare(const PrepareSnapshotQueryRequest &r) {
    if (static_cast<size_t>(r.bindings_size()) != result.reads.size())
      refuse("scratch bindings must be an exact ordered bijection", INVALID_INPUT);
    std::map<std::string, std::pair<std::string, std::string>> bindings;
    std::set<std::pair<std::string, std::string>> used;
    size_t i = 0;
    for (auto &id : result.reads) {
      auto &b = r.bindings(i++);
      auto p = std::make_pair(b.scratch_database(), b.scratch_table());
      auto nonblank = [](const std::string &s) {
        return nameValid(s) && s.find_first_not_of(" \t\r\n") != std::string::npos;
      };
      if (b.table_id() != id || !nonblank(p.first) || !nonblank(p.second) || !used.insert(p).second)
        refuse("scratch bindings must be an exact ordered bijection", INVALID_INPUT);
      bindings[id] = p;
    }
    check(execution, limits);
    for (auto &w : withs) {
      DB::ASTs kept;
      for (auto &d : w.definitions)
        if (result.used.contains(d)) kept.push_back(d->node);
      w.list->children = std::move(kept);
      if (w.list->children.empty()) {
        if (w.owner)
          w.owner->setExpression(DB::ASTSelectQuery::Expression::WITH, {});
        else
          outer.reset();
      }
    }
    for (auto &b : result.bases) {
      auto pair = bindings.at(b->table->table_id());
      auto *id = b->node->as<DB::ASTTableIdentifier>();
      if (id->tryGetAlias().empty() && (b->qualified || b->other.contains(pair.second)))
        id->setAlias(b->qualifier);
      id->resetTable(pair.first, pair.second);
    }
    for (auto *s : result.leaves) {
      auto list = s->select();
      auto old = list->children;
      DB::ASTs ordered;
      for (size_t i = 0; i < order.size(); ++i) {
        auto j = order[i];
        auto n = old[j];
        auto &v = result.columns[j];
        if (result.leaves.size() == 1 && ((!v.number.empty() && !v.nullable) || v.scalar)) {
          auto alias = n->tryGetAlias();
          n->setAlias("");
          n = DB::makeASTFunction("accurateCast",
            v.scalar ? n : stringNode(v.number),
            stringNode(destination->columns(i).type()));
          n->setAlias(alias);
        }
        ordered.push_back(n);
      }
      list->children = std::move(ordered);
    }
    auto body = root->as<DB::ASTInsertQuery>()->select;
    if (outer) {
      auto *u = body->as<DB::ASTSelectWithUnionQuery>();
      auto *s = u && u->list_of_selects->children.size() == 1
                  ? u->list_of_selects->children[0]->as<DB::ASTSelectQuery>()
                  : nullptr;
      if (s && !s->with())
        s->setExpression(DB::ASTSelectQuery::Expression::WITH, ASTPtr(outer));
      else {
        auto wrapper = parse("SELECT * FROM (SELECT 1) AS snapshot_output");
        auto *select = wrapper->as<DB::ASTSelectWithUnionQuery>()
                         ->list_of_selects->children[0]
                         ->as<DB::ASTSelectQuery>();
        select->setExpression(DB::ASTSelectQuery::Expression::WITH, ASTPtr(outer));
        auto *el = select->tables()->children[0]->as<DB::ASTTablesInSelectQueryElement>();
        el->table_expression->as<DB::ASTTableExpression>()->subquery->children[0] = body;
        body = wrapper;
      }
    }
    return generate(body, execution, limits);
  }
};

template <class Response> void failure(Response *out, const Refusal &r) {
  auto v = out->contract_version();
  auto q = out->query_profile_id();
  out->Clear();
  out->set_contract_version(v);
  out->set_query_profile_id(q);
  out->set_code(r.code);
  out->set_message(r.message);
}
template <class Response>
Limits admission(const snapshot_profiles::Profiles *profiles,
  const AnalyzeSnapshotQueryRequest *r,
  Response *out,
  Execution &e) {
  out->Clear();
  if (!r || r->contract_version() != 1) refuse("contract version is unsupported", INVALID_INPUT);
  Limits l;
  if (!supported(profiles ? profiles->FindCandidate(r->query_profile_id()) : nullptr, l))
    refuse("profile is unavailable", PROFILE_UNAVAILABLE);
  out->set_contract_version(1);
  out->set_query_profile_id(r->query_profile_id());
  check(e, l);
  if (r->sql().size() > l.sql) refuse("SQL exceeds the profile limit", INVALID_INPUT);
  if (r->ByteSizeLong() > l.descriptor + r->sql().size())
    refuse("descriptor exceeds the profile limit", INVALID_INPUT);
  if (!r->materialize() && r->has_inputs()) refuse("inputs require materialization", INVALID_INPUT);
  for (auto &t : r->catalog())
    if (!digest(t.table_id()) || !digest(t.schema_hash()))
      refuse("catalog identity is invalid", INVALID_INPUT);
  return l;
}
void checkInputs(const AnalyzeSnapshotQueryRequest &r) {
  if (r.has_inputs() && (r.inputs().has_now_unix_ns() || r.inputs().random_float64_values_size() ||
                          r.inputs().uuid_values_size()))
    refuse("materialization is incomplete or unsupported", MATERIALIZATION_FAILED);
}
void checkSQL(const std::string &s, const Limits &l) {
  if (s.size() > l.sql) refuse("SQL exceeds the profile limit", INVALID_INPUT);
}
} // namespace
} // namespace snapshot_query

void RewriterServiceImpl::doAnalyzeSnapshotQuery(const rewriter::AnalyzeSnapshotQueryRequest *r,
  rewriter::AnalyzeSnapshotQueryResponse *out,
  snapshot_query::Execution e) {
  using namespace snapshot_query;
  try {
    auto l = admission(SnapshotCandidates(), r, out, e);
    Analyzer a(*r, e, l);
    a.analyze();
    check(e, l);
    checkInputs(*r);
    if (a.ordinary) {
      out->set_code(rewriter::NOT_SNAPSHOT_QUERY);
      return;
    }
    auto sql = a.logical();
    checkSQL(sql, l);
    check(e, l);
    out->set_code(rewriter::SUCCESS);
    out->set_sql_after_materialization(sql);
    out->set_target_table_id(a.destination->table_id());
    for (auto &n : a.target_names) out->add_target_columns(n);
    for (auto &id : a.result.reads) out->add_read_table_ids(id);
  } catch (const Refusal &r) { failure(out, r); } catch (...) {
    failure(
      out, Refusal{rewriter::INVALID_INPUT, "snapshot query requires one recognized statement"});
  }
}
void RewriterServiceImpl::doPrepareSnapshotQuery(const rewriter::PrepareSnapshotQueryRequest *r,
  rewriter::PrepareSnapshotQueryResponse *out,
  snapshot_query::Execution e) {
  using namespace snapshot_query;
  out->Clear();
  try {
    if (!r || !r->has_analysis()) refuse("preparation requires analysis", rewriter::INVALID_INPUT);
    auto l = admission(SnapshotCandidates(), &r->analysis(), out, e);
    if (r->analysis().materialize())
      refuse("preparation requires nonmaterializing analysis", rewriter::INVALID_INPUT);
    if (r->ByteSizeLong() > l.descriptor + r->analysis().sql().size())
      refuse("descriptor exceeds the profile limit", rewriter::INVALID_INPUT);
    Analyzer a(r->analysis(), e, l);
    a.analyze();
    checkInputs(r->analysis());
    if (a.ordinary) refuse("preparation requires an insert select", rewriter::INVALID_INPUT);
    checkSQL(a.logical(), l);
    auto sql = a.prepare(*r);
    checkSQL(sql, l);
    check(e, l);
    out->set_code(rewriter::SUCCESS);
    out->set_select_sql(sql);
    out->set_target_table_id(a.destination->table_id());
    for (auto &c : a.destination->columns()) out->add_target_columns(c.name());
    for (auto &id : a.result.reads) out->add_read_table_ids(id);
  } catch (const Refusal &r) { failure(out, r); } catch (...) {
    failure(
      out, Refusal{rewriter::INVALID_INPUT, "snapshot query requires one recognized statement"});
  }
}
