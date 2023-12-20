/******************************************************************************\

          This file is part of the C! library.  A.K.A the cbang library.

                Copyright (c) 2003-2023, Cauldron Development LLC
                               All rights reserved.

         The C! library is free software: you can redistribute it and/or
        modify it under the terms of the GNU Lesser General Public License
       as published by the Free Software Foundation, either version 2.1 of
               the License, or (at your option) any later version.

        The C! library is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
                 Lesser General Public License for more details.

         You should have received a copy of the GNU Lesser General Public
                 License along with the C! library.  If not, see
                         <http://www.gnu.org/licenses/>.

        In addition, BSD licensing may be granted on a case by case basis
        by written permission from at least one of the copyright holders.
           You may request written permission by emailing the authors.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include "Path.h"
#include "Value.h"

#include <cbang/String.h>
#include <cbang/log/Logger.h>

using namespace std;
using namespace cb;
using namespace cb::JSON;


Path::Path(const string &path) {
  String::tokenize(path, parts, ".");
  if (parts.empty()) THROW("JSON Path cannot be empty");
}


string Path::toString(unsigned start, unsigned end) const {
  if (end < 0) end = size() + end + 1;
  vector<string> v(parts.begin() + start, parts.begin() + end);
  return cb::String::join(v, ".");
}


string Path::pop() {
  if (empty()) THROW("Cannot pop from empty JSON::Path");
  string part = parts.back();
  parts.pop_back();
  return part;
}


void Path::push(const string &part) {parts.push_back(part);}


ValuePtr Path::select(const Value &value, fail_cb_t fail_cb) const {
  ValuePtr ptr;
  unsigned i;

  for (i = 0; i < size(); i++) {
    const Value &v = i ? *ptr : value;

    int index = -1;

    if (v.isList())
      try {
        index = String::parseU32(parts[i], true);
      } catch (const Exception &e) {}

    else if (v.isDict()) index = v.indexOf(parts[i]);

    if (index == -1 || (int)v.size() <= index) break;
    ptr = v.get(index);
  }

  if (i == size()) return ptr;

  if (fail_cb) return fail_cb(i);

  CBANG_KEY_ERROR("At JSON path: " << toString(0, i + 1));
}


ValuePtr Path::select(const Value &value, const ValuePtr &defaultValue) const {
  auto cb = [&] (unsigned i) {return defaultValue;};
  return select(value, cb);
}


bool Path::exists(const Value &value) const {
  auto cb = [] (unsigned i) {return (ValuePtr)0;};
  return select(value, cb).isSet();
}


void Path::insert(Value &target, const ValuePtr &value) {
  ValuePtr v = SmartPointer<Value>::Phony(&target);

  if (1 < parts.size()) {
    Path path(parts.begin(), parts.begin() + parts.size() - 1);
    v = path.select(target);
  }

  string key = parts.back();

  if (v->isList())
    try {
      int index = String::parseS32(key, true);
      if (index == -1 || index == (int)v->size()) v->append(value);
      else v->set(index, value);

    } catch (const Exception &e) {
      CBANG_KEY_ERROR("At JSON path: " << toString());
    }

  else if (v->isDict()) v->insert(key, value);
}


#define CBANG_JSON_VT(NAME, TYPE)                                   \
  TYPE Path::select##NAME(const Value &value) const {               \
    ValuePtr result = select(value);                                \
                                                                    \
    if (!result->is##NAME())                                        \
      CBANG_TYPE_ERROR("Not a " #NAME " at " << toString());        \
                                                                    \
    return result->get##NAME();                                     \
  }                                                                 \
                                                                    \
                                                                    \
  TYPE Path::select##NAME(const Value &value,                       \
                          TYPE defaultValue) const {                \
    ValuePtr result = select(value, ValuePtr());                    \
                                                                    \
    if (result.isNull() || !result->is##NAME())                     \
      return defaultValue;                                          \
                                                                    \
    return result->get##NAME();                                     \
  }                                                                 \
                                                                    \
                                                                    \
  bool Path::exists##NAME(const Value &value) const {               \
    auto cb = [] (unsigned i) {return (ValuePtr)0;};                \
    ValuePtr result = select(value, cb);                            \
    return result.isSet() && result->is##NAME();                    \
  }
#include "ValueTypes.def"
