/******************************************************************************\

          This file is part of the C! library.  A.K.A the cbang library.

                Copyright (c) 2003-2024, Cauldron Development LLC
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

#include "OptionCategory.h"

#include <cbang/String.h>
#include <cbang/json/Sink.h>

using namespace std;
using namespace cb;


bool OptionCategory::hasSetOption() const {
  for (auto &p: options)
    if (p.second->isSet()) return true;

  return false;
}


void OptionCategory::add(const SmartPointer<Option> &option) {
  options.insert(options_t::value_type(option->getName(), option));
}


void OptionCategory::write(JSON::Sink &sink, bool config,
                           const string &delims) const {
  if (!config) sink.beginDict();

  for (auto &p: options) {
    Option &option = *p.second;

    if (config && !option.isSet()) continue;

    if (!option.isHidden()) {
      sink.beginInsert(option.getName());
      option.write(sink, config, delims);
    }
  }

  if (!config) sink.endDict();
}


void OptionCategory::write(XML::Handler &handler, uint32_t flags) const {
  bool first = true;

  for (auto &p: options) {
    Option &option = *p.second;

    // Don't print options set on the command line
    if (option.isCommandLine() && !(flags & Option::COMMAND_LINE_FLAG) &&
        (!option.hasDefault() || !(flags & Option::DEFAULT_SET_FLAG)))
      continue;

    if (((flags & Option::DEFAULT_SET_FLAG) && option.hasValue()) ||
        (option.isSet() && !option.isDefault())) {
      if (first) {
        first = false;
        if (!name.empty()) handler.comment(name);
      }

      option.write(handler, flags);
    }
  }

  if (!first) handler.text("\n");
}


void OptionCategory::printHelpTOC(XML::Handler &handler,
                                  const string &prefix) const {
  if (options.empty()) return;

  XML::Attributes attrs;

  attrs["class"] = "option-category";
  handler.startElement("li", attrs);

  string catName = name.empty() ? "Uncategorized" : name;
  attrs.clear();
  attrs["href"] = "#" + prefix + "option-category-" + catName;
  handler.startElement("a", attrs);
  handler.text(catName);
  handler.endElement("a");

  handler.startElement("ul");

  for (auto &p: options)
    if (!p.second->isHidden())
      p.second->printHelpTOC(handler, prefix);

  handler.endElement("ul");
  handler.endElement("li");
}


void OptionCategory::printHelp(XML::Handler &handler,
                               const string &prefix) const {
  if (options.empty()) return;

  XML::Attributes attrs;

  attrs["class"] = "option-category";
  attrs["id"] = prefix + "option-category-" + name;
  handler.startElement("div", attrs);

  if (!name.empty()) {
    attrs["class"] = "option-category-name";
    handler.startElement("div", attrs);
    handler.text(name);
    handler.endElement("div");
  }

  if (!description.empty()) {
    attrs["class"] = "option-category-description";
    handler.startElement("div", attrs);
    handler.text(description);
    handler.endElement("div");
  }

  for (auto &p: options)
    if (!p.second->isHidden())
      p.second->printHelp(handler, prefix);

  handler.endElement("div");
}


void OptionCategory::printHelp(ostream &stream, bool cmdLine) const {
  if (!name.empty()) stream << name << ":\n";
  if (!description.empty()) stream << description << "\n";

  bool first = true;
  for (auto &p: options)
    if (!p.second->isHidden()) {
      if (first) first = false;
      else stream << "\n\n";
      p.second->printHelp(stream, cmdLine);
    }
}
