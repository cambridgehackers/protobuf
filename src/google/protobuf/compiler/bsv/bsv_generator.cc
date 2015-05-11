// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Edited from: src/google/protobuf/compiler/python/python_generator.cc,
// which is copyright:
//     Copyright 2007 Google Inc. All Rights Reserved.
//     Author: robinson@google.com (Will Robinson)
//
// Copyright 2015 The Connectal Project.
//
// This module outputs pure-BSV protocol message classes that will
// largely be constructed at runtime via the metaclass in reflection.py.
// In other words, our job is basically to output a BSV equivalent
// of the C++ *Descriptor objects, and fix up all circular references
// within these objects.
//

#include <google/protobuf/stubs/hash.h>
#include <limits>
#include <map>
#include <memory>
#ifndef _SHARED_PTR_H
#include <google/protobuf/stubs/shared_ptr.h>
#endif
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/bsv/bsv_generator.h>
#include <google/protobuf/descriptor.pb.h>

#include <google/protobuf/stubs/common.h>
#include <google/protobuf/stubs/stringprintf.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/stubs/strutil.h>
#include <google/protobuf/stubs/substitute.h>

namespace google {
namespace protobuf {
namespace compiler {
namespace bsv {

namespace {

// Returns a copy of |filename| with any trailing ".protodevel" or ".proto
// suffix stripped.
// TODO(robinson): Unify with copy in compiler/cpp/internal/helpers.cc.
string StripProto(const string& filename) {
  const char* suffix = HasSuffixString(filename, ".protodevel")
      ? ".protodevel" : ".proto";
  return StripSuffixString(filename, suffix);
}


// Returns the BSV module name expected for a given .proto filename.
string ModuleName(const string& filename) {
  string basename = StripProto(filename);
  StripString(&basename, "-", '_');
  StripString(&basename, "/", '.');
  return basename + "_pb";
}


// Returns the alias we assign to the module of the given .proto filename
// when importing. See testPackageInitializationImport in
// google/protobuf/bsv/reflection_test.py
// to see why we need the alias.
string ModuleAlias(const string& filename) {
  string module_name = ModuleName(filename);
  // We can't have dots in the module name, so we replace each with _dot_.
  // But that could lead to a collision between a.b and a_dot_b, so we also
  // duplicate each underscore.
  GlobalReplaceSubstring("_", "__", &module_name);
  GlobalReplaceSubstring(".", "_dot_", &module_name);
  return module_name;
}


// Returns an import statement of form "from X.Y.Z import T" for the given
// .proto filename.
string ModuleImportStatement(const string& filename) {
  string module_name = ModuleName(filename);
  int last_dot_pos = module_name.rfind('.');
  if (last_dot_pos == string::npos) {
    // NOTE(petya): this is not tested as it would require a protocol buffer
    // outside of any package, and I don't think that is easily achievable.
    return "import " + module_name;
  } else {
    return "from " + module_name.substr(0, last_dot_pos) + " import " +
        module_name.substr(last_dot_pos + 1);
  }
}


// Returns the name of all containing types for descriptor,
// in order from outermost to innermost, followed by descriptor's
// own name.  Each name is separated by |separator|.
template <typename DescriptorT>
string NamePrefixedWithNestedTypes(const DescriptorT& descriptor,
                                   const string& separator) {
  string name = descriptor.name();
  for (const Descriptor* current = descriptor.containing_type();
       current != NULL; current = current->containing_type()) {
    name = current->name() + separator + name;
  }
  return name;
}


// Name of the class attribute where we store the BSV
// descriptor.Descriptor instance for the generated class.
// Must stay consistent with the _DESCRIPTOR_KEY constant
// in proto2/public/reflection.py.
const char kDescriptorKey[] = "DESCRIPTOR";
#if 0
// Should we generate generic services for this file?
inline bool HasGenericServices(const FileDescriptor *file) {
  return file->service_count() > 0 &&
         false; //file->options().bsv_generic_services();
}
#endif

// Returns a BSV literal giving the default value for a field.
// If the field specifies no explicit default value, we'll return
// the default default value for the field type (zero for numbers,
// empty string for strings, empty list for repeated fields, and
// None for non-repeated, composite fields).
//
// TODO(robinson): Unify with code from
// //compiler/cpp/internal/primitive_field.cc
// //compiler/cpp/internal/enum_field.cc
// //compiler/cpp/internal/string_field.cc
string StringifyDefaultValue(const FieldDescriptor& field) {
  if (field.is_repeated()) {
    return "[]";
  }

  switch (field.cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      return SimpleItoa(field.default_value_int32());
    case FieldDescriptor::CPPTYPE_UINT32:
      return SimpleItoa(field.default_value_uint32());
    case FieldDescriptor::CPPTYPE_INT64:
      return SimpleItoa(field.default_value_int64());
    case FieldDescriptor::CPPTYPE_UINT64:
      return SimpleItoa(field.default_value_uint64());
    case FieldDescriptor::CPPTYPE_DOUBLE: {
      double value = field.default_value_double();
      if (value == numeric_limits<double>::infinity()) {
        // BSV pre-2.6 on Windows does not parse "inf" correctly.  However,
        // a numeric literal that is too big for a double will become infinity.
        return "1e10000";
      } else if (value == -numeric_limits<double>::infinity()) {
        // See above.
        return "-1e10000";
      } else if (value != value) {
        // infinity * 0 = nan
        return "(1e10000 * 0)";
      } else {
        return SimpleDtoa(value);
      }
    }
    case FieldDescriptor::CPPTYPE_FLOAT: {
      float value = field.default_value_float();
      if (value == numeric_limits<float>::infinity()) {
        // BSV pre-2.6 on Windows does not parse "inf" correctly.  However,
        // a numeric literal that is too big for a double will become infinity.
        return "1e10000";
      } else if (value == -numeric_limits<float>::infinity()) {
        // See above.
        return "-1e10000";
      } else if (value != value) {
        // infinity - infinity = nan
        return "(1e10000 * 0)";
      } else {
        return SimpleFtoa(value);
      }
    }
    case FieldDescriptor::CPPTYPE_BOOL:
      return field.default_value_bool() ? "True" : "False";
    case FieldDescriptor::CPPTYPE_ENUM:
      return SimpleItoa(field.default_value_enum()->number());
    case FieldDescriptor::CPPTYPE_STRING:
      return "_b(\"" + CEscape(field.default_value_string()) +
             (field.type() != FieldDescriptor::TYPE_STRING ? "\")" :
               "\").decode('utf-8')");
    case FieldDescriptor::CPPTYPE_MESSAGE:
      return "None";
  }
  // (We could add a default case above but then we wouldn't get the nice
  // compiler warning when a new type is added.)
  GOOGLE_LOG(FATAL) << "Not reached.";
  return "";
}

string StringifySyntax(FileDescriptor::Syntax syntax) {
  switch (syntax) {
    case FileDescriptor::SYNTAX_PROTO2:
      return "proto2";
    case FileDescriptor::SYNTAX_PROTO3:
      return "proto3";
    case FileDescriptor::SYNTAX_UNKNOWN:
    default:
      GOOGLE_LOG(FATAL) << "Unsupported syntax; this generator only supports proto2 and proto3 syntax.";
      return "";
  }
}


}  // namespace


Generator::Generator() : file_(NULL) {
}

Generator::~Generator() {
}

static const char *enum_separator = " ";
bool Generator::Generate(const FileDescriptor* file,
                         const string& parameter,
                         GeneratorContext* context,
                         string* error) const {

  // Completely serialize all Generate() calls on this instance.  The
  // thread-safety constraints of the CodeGenerator interface aren't clear so
  // just be as conservative as possible.  It's easier to relax this later if
  // we need to, but I doubt it will be an issue.
  // TODO(kenton):  The proper thing to do would be to allocate any state on
  //   the stack and use that, so that the Generator class itself does not need
  //   to have any mutable members.  Then it is implicitly thread-safe.
  MutexLock lock(&mutex_);
  file_ = file;
  string module_name = ModuleName(file->name());
  const char *separator = " ";
  string filename = module_name;
  StripString(&filename, ".", '/');
  filename += ".json";

  FileDescriptorProto fdp;
  file_->CopyTo(&fdp);
  fdp.SerializeToString(&file_descriptor_serialized_); 
  google::protobuf::scoped_ptr<io::ZeroCopyOutputStream> output(context->Open(filename));
  GOOGLE_CHECK(output.get());
  io::Printer printer(output.get(), '$');
  printer_ = &printer;

  //printer_->Print("// Generated by the protocol buffer compiler.  DO NOT EDIT!\n// source: $filename$\n\n",
  //    "filename", file_->name());
  printer_->Print("{\n    \"globaldecls\": [\n");
  PrintImports();
  vector<pair<string, int> > top_level_enum_values;
  for (int i = 0; i < file_->enum_type_count(); ++i) {
    const EnumDescriptor& enum_descriptor = *file_->enum_type(i);
    printer_->Print("KKLK");
    printer_->Print(separator);
    PrintEnum(enum_descriptor);
    printer_->Print("$name$ = enum_type_wrapper.EnumTypeWrapper($descriptor_name$)",
        "name", enum_descriptor.name(), "descriptor_name", ModuleLevelDescriptorName(enum_descriptor));

    for (int j = 0; j < enum_descriptor.value_count(); ++j) {
      const EnumValueDescriptor& value_descriptor = *enum_descriptor.value(j);
      top_level_enum_values.push_back( std::make_pair(value_descriptor.name(), value_descriptor.number()));
    }
    separator = ",";
  }
  for (int i = 0; i < top_level_enum_values.size(); ++i) {
    printer_->Print("$name$ = $value$\n", "name", top_level_enum_values[i].first, "value", SimpleItoa(top_level_enum_values[i].second));
  }
  const bool top_is_extension = true;
  for (int i = 0; i < file_->extension_count(); ++i) {
    const FieldDescriptor& extension_field = *file_->extension(i);
    string constant_name = extension_field.name() + "_FIELD_NUMBER";
    UpperString(&constant_name);
    printer_->Print("KMMM");
    printer_->Print(separator);
    printer_->Print("$constant_name$ = $number$\n", "constant_name", constant_name, "number", SimpleItoa(extension_field.number()));
    printer_->Print("$name$ = ", "name", extension_field.name());
    PrintFieldDescriptor(extension_field, top_is_extension);
    separator = ",";
  }
  enum_separator = " ";
  for (int i = 0; i < file_->message_type_count(); ++i) {
    //printer_->Print("KRRR");
    //printer_->Print(separator);
    PrintNestedEnums(*file_->message_type(i));
  }
  separator = enum_separator;
  for (int i = 0; i < file_->message_type_count(); ++i) {
    if (!strcmp(file_->message_type(i)->name().c_str(), "Empty"))
       continue;
    printer_->Print(separator);
    PrintDescriptor(*file_->message_type(i));
    separator = ",";
  }
  printer_->Print("    ],\n    \"interfaces\": [\n");
  FixAllDescriptorOptions();
  for (int outeri = 0; outeri < file_->service_count(); ++outeri) {
    string service_name = ModuleLevelServiceDescriptorName(*file_->service(outeri));
    string options_string;
    file_->service(outeri)->options().SerializeToString(&options_string);
    map<string, string> m;
    m["name"] = file_->service(outeri)->name();
    m["index"] = SimpleItoa(file_->service(outeri)->index());
    m["options_value"] = OptionsValue("ServiceOptions", options_string);
    if (outeri)
      printer_->Print(",");
    printer_->Print(m, "        { \"cname\": \"$name$\", \"cdecls\": [\n");
    printer_->Indent();
    //ServiceDescriptorProto sdp;
    //PrintSerializedPbInterval(descriptor, sdp);
    const char *separator = " ";
    for (int inneri = 0; inneri < file_->service(outeri)->method_count(); ++inneri) {
      const MethodDescriptor* method = file_->service(outeri)->method(inneri);
      method->options().SerializeToString(&options_string);

      printer_->Print(separator);
      m.clear();
      m["name"] = method->name();
      m["full_name"] = method->full_name();
      m["index"] = SimpleItoa(method->index());
      m["serialized_options"] = CEscape(options_string);
      m["input_type"] = method->input_type()->name();
      m["options_value"] = OptionsValue("MethodOptions", options_string);
      printer_->Print( m,
          "               { \"dname\": \"$name$\", \"dparams\": [\n"
          "                    { \"pname\": \"v\", \"ptype\": { \"name\": \"$input_type$\"} }]\n"
                    //{ "pname": "v", "ptype": { "name": "Bit", "params": [ { "name": "32" } ] } }]
          "                }\n");
      separator = ",";
    }
    printer_->Outdent();
    printer_->Print( "            ]\n        }\n");
  }
  printer_->Print("    ]\n}\n");
  return !printer.failed();
}

// Prints BSV imports for all modules imported by |file|.
void Generator::PrintImports() const {
  for (int i = 0; i < file_->dependency_count(); ++i) {
    const string& filename = file_->dependency(i)->name();
    string import_statement = ModuleImportStatement(filename);
    string module_alias = ModuleAlias(filename);
    printer_->Print("$statement$ as $alias$\n", "statement", import_statement, "alias", module_alias);
    CopyPublicDependenciesAliases(module_alias, file_->dependency(i));
  }
  // Print public imports.
  for (int i = 0; i < file_->public_dependency_count(); ++i) {
    string module_name = ModuleName(file_->public_dependency(i)->name());
    printer_->Print("from $module$ import *\n", "module", module_name);
  }
}

void Generator::PrintEnum(const EnumDescriptor& enum_descriptor) const {
  string module_level_descriptor_name =
      ModuleLevelDescriptorName(enum_descriptor);
  string options_string;
  enum_descriptor.options().SerializeToString(&options_string);
  printer_->Print(
      "        { \"dtype\": \"TypeDef\", \"tname\": \"$name$\",\n"
      "            \"tdtype\": {\n"
      "                \"elements\": [ ", "name", enum_descriptor.name());
  printer_->Indent();
  for (int i = 0; i < enum_descriptor.value_count(); ++i) {
    string options_string;
    enum_descriptor.value(i)->options().SerializeToString(&options_string);
    map<string, string> m;
    m["name"] = enum_descriptor.value(i)->name();
    m["index"] = SimpleItoa(enum_descriptor.value(i)->index());
    m["number"] = SimpleItoa(enum_descriptor.value(i)->number());
    m["options"] = OptionsValue("EnumValueOptions", options_string);
    if (i)
        printer_->Print( m, ",");
    printer_->Print( m, "\"$name$\"");
  }
  printer_->Outdent();
  printer_->Print( " ], \n"
      "                \"name\": \"$name$\", \n"
      "                \"type\": \"Enum\"\n"
      "            }\n"
      "        }\n" , "name", enum_descriptor.name());
enum_separator = ",";
}

// Recursively prints enums in nested types within descriptor, then
// prints enums contained at the top level in descriptor.
void Generator::PrintNestedEnums(const Descriptor& descriptor) const {
  for (int i = 0; i < descriptor.nested_type_count(); ++i) {
    PrintNestedEnums(*descriptor.nested_type(i));
  }
  for (int i = 0; i < descriptor.enum_type_count(); ++i) {
    PrintEnum(*descriptor.enum_type(i));
  }
}


void Generator::PrintDescriptorKeyAndModuleName(
    const ServiceDescriptor& descriptor) const {
  printer_->Print(
      "$descriptor_key$ = $descriptor_name$,",
      "descriptor_key", kDescriptorKey,
      "descriptor_name", ModuleLevelServiceDescriptorName(descriptor));
  //printer_->Print( "__module__ = '$module_name$'\n", "module_name", ModuleName(file_->name()));
}

// Prints statement assigning ModuleLevelDescriptorName(message_descriptor)
// to a BSV Descriptor object for message_descriptor.
//void Generator::PrintNestedDescriptors(const Descriptor& containing_descriptor) const {
//}
void Generator::PrintDescriptor(const Descriptor& message_descriptor) const {
  const char *separator = " ";
  //PrintNestedDescriptors(message_descriptor);
  for (int i = 0; i < message_descriptor.nested_type_count(); ++i) {
    if (!strcmp(message_descriptor.nested_type(i)->name().c_str(), "Empty"))
       continue;
    printer_->Print(separator);
    PrintDescriptor(*message_descriptor.nested_type(i));
    separator = ",";
  }

  //printer_->Print("\n$descriptor_name$ = DESC(\n", "descriptor_name", ModuleLevelDescriptorName(message_descriptor));
  //printer_->Indent();
  map<string, string> m;
  m["name"] = message_descriptor.name();
  m["full_name"] = message_descriptor.full_name();
  m["file"] = kDescriptorKey;
  const bool is_extension = false;
  if (!strcmp(message_descriptor.name().c_str(), "Empty")) {
      return;   // don't print this definition
  }
  printer_->Print(separator);
  printer_->Print(m,
      "        {\n"
      "            \"dtype\": \"TypeDef\", \n"
      "            \"tdtype\": {\n"
      "                \"elements\": [\n");
  printer_->Indent();
  const char *fseparator = " ";
  for (int i = 0; i < (message_descriptor.field_count)(); ++i) {
    printer_->Print(fseparator);
    PrintFieldDescriptor(*(message_descriptor.field)(i), is_extension);
    fseparator = ",";
  }
  printer_->Print(m, 
      "                ], \n"
      "                \"name\": \"$name$\", \n"
      "                \"type\": \"Struct\"\n"
      "            }, \n"
      "            \"tname\": \"$name$\"\n"
      "      }\n");

  // Extension ranges
  if (message_descriptor.extension_range_count()) {
  printer_->Print("extension_ranges=[");
  for (int i = 0; i < message_descriptor.extension_range_count(); ++i) {
    const Descriptor::ExtensionRange* range = message_descriptor.extension_range(i);
    printer_->Print("($start$, $end$), ", "start", SimpleItoa(range->start), "end", SimpleItoa(range->end));
  }
  printer_->Print("],");
  }
  if (message_descriptor.oneof_decl_count()) {
  printer_->Print("oneofs=[");
  printer_->Indent();
  for (int i = 0; i < message_descriptor.oneof_decl_count(); ++i) {
    const OneofDescriptor* desc = message_descriptor.oneof_decl(i);
    map<string, string> m;
    m["name"] = desc->name();
    m["full_name"] = desc->full_name();
    m["index"] = SimpleItoa(desc->index());
    printer_->Print( m, "OOD(name='$name$'),");
  }
  printer_->Outdent();
  printer_->Print("],");
  }
  // Serialization of proto
  //DescriptorProto edp;
  //PrintSerializedPbInterval(message_descriptor, edp); 
  printer_->Outdent();
  //printer_->Print(";\n");
}

void Generator::AddMessageToFileDescriptor(const Descriptor& descriptor) const {
  map<string, string> m;
  m["descriptor_name"] = kDescriptorKey;
  m["message_name"] = descriptor.name();
  m["message_descriptor_name"] = ModuleLevelDescriptorName(descriptor);
  const char file_descriptor_template[] =
      "$descriptor_name$.message_types_by_name['$message_name$'] = $message_descriptor_name$\n";
  printer_->Print(m, file_descriptor_template);
}

void Generator::AddEnumToFileDescriptor(
    const EnumDescriptor& descriptor) const {
  map<string, string> m;
  m["descriptor_name"] = kDescriptorKey;
  m["enum_name"] = descriptor.name();
  m["enum_descriptor_name"] = ModuleLevelDescriptorName(descriptor);
  const char file_descriptor_template[] =
      "$descriptor_name$.enum_types_by_name['$enum_name$'] = $enum_descriptor_name$\n";
  printer_->Print(m, file_descriptor_template);
}

void Generator::AddExtensionToFileDescriptor(
    const FieldDescriptor& descriptor) const {
  map<string, string> m;
  m["descriptor_name"] = kDescriptorKey;
  m["field_name"] = descriptor.name();
  const char file_descriptor_template[] =
      "$descriptor_name$.extensions_by_name['$field_name$'] = $field_name$\n";
  printer_->Print(m, file_descriptor_template);
}

// Sets any necessary message_type and enum_type attributes
// for the BSV version of |field|.
//
// containing_type may be NULL, in which case this is a module-level field.
//
// bsv_dict_name is the name of the BSV dict where we should
// look the field up in the containing type.  (e.g., fields_by_name
// or extensions_by_name).  We ignore bsv_dict_name if containing_type
// is NULL.
void Generator::FixForeignFieldsInField(const Descriptor* containing_type,
                                        const FieldDescriptor& field,
                                        const string& bsv_dict_name) const {
  const string field_referencing_expression = FieldReferencingExpression(
      containing_type, field, bsv_dict_name);
  map<string, string> m;
  m["field_ref"] = field_referencing_expression;
  const Descriptor* foreign_message_type = field.message_type();
  if (foreign_message_type) {
    m["foreign_type"] = ModuleLevelDescriptorName(*foreign_message_type);
    printer_->Print(m, "$field_ref$.message_type = $foreign_type$\n");
  }
  const EnumDescriptor* enum_type = field.enum_type();
  if (enum_type) {
    m["enum_type"] = ModuleLevelDescriptorName(*enum_type);
    printer_->Print(m, "$field_ref$.enum_type = $enum_type$\n");
  }
}

// Returns the module-level expression for the given FieldDescriptor.
// Only works for fields in the .proto file this Generator is generating for.
//
// containing_type may be NULL, in which case this is a module-level field.
//
// bsv_dict_name is the name of the BSV dict where we should
// look the field up in the containing type.  (e.g., fields_by_name
// or extensions_by_name).  We ignore bsv_dict_name if containing_type
// is NULL.
string Generator::FieldReferencingExpression( const Descriptor* containing_type,
    const FieldDescriptor& field, const string& bsv_dict_name) const {
  // We should only ever be looking up fields in the current file.
  // The only things we refer to from other files are message descriptors.
  GOOGLE_CHECK_EQ(field.file(), file_) << field.file()->name() << " vs. " << file_->name();
  if (!containing_type) {
    return field.name();
  }
  return strings::Substitute( "$0.$1['$2']",
      ModuleLevelDescriptorName(*containing_type), bsv_dict_name, field.name());
}

// Returns a BSV expression that calls descriptor._ParseOptions using
// the given descriptor class name and serialized options protobuf string.
string Generator::OptionsValue( const string& class_name, const string& serialized_options) const {
  if (serialized_options.length() == 0 || GeneratingDescriptorProto()) {
    return "None";
  } else {
    string full_class_name = "descriptor_pb2." + class_name;
    return "_descriptor._ParseOptions(" + full_class_name + "(), _b('" + CEscape(serialized_options)+ "'))";
  }
}

// Prints an expression for a BSV FieldDescriptor for |field|.
void Generator::PrintFieldDescriptor(const FieldDescriptor& field, bool is_extension) const
{
  string options_string;
  field.options().SerializeToString(&options_string);
  map<string, string> m;
  m["name"] = field.name();
  m["full_name"] = field.full_name();
  m["index"] = SimpleItoa(field.index());
  m["number"] = SimpleItoa(field.number());
  m["type"] = SimpleItoa(field.type());
  const Descriptor *mt = field.message_type();
  const EnumDescriptor *et = field.enum_type();
  if (mt)
    m["cpp_type"] = mt->name();
  else if (et)
    m["cpp_type"] = et->name();
  else
    m["cpp_type"] = field.type_name();
  m["label"] = SimpleItoa(field.label());
  m["has_default_value"] = field.has_default_value() ? "True" : "False";
  m["default_value"] = StringifyDefaultValue(field);
  m["is_extension"] = is_extension ? "True" : "False";
  m["options"] = OptionsValue("FieldOptions", options_string);
  //printer_->Print(m, "FD(name='$name$',number=$number$,cpp=$cpp_type$,label=$label$)\n");
  //{ \"pname\": \"b\", \"ptype\": { \"name\": \"Bit\", \"params\": [ { \"name\": \"16\" } ] } }
  //"$cpp_type$ $name$; /*number=$number$,label=$label$*/\n"
  printer_->Print(m, "                    { \"pname\": \"$name$\", \"ptype\": { \"name\": \"$cpp_type$\"} }\n");
}

bool Generator::GeneratingDescriptorProto() const {
  return file_->name() == "google/protobuf/descriptor.proto";
}

// Returns the unique BSV module-level identifier given to a descriptor.
// This name is module-qualified iff the given descriptor describes an
// entity that doesn't come from the current file.
template <typename DescriptorT>
string Generator::ModuleLevelDescriptorName(const DescriptorT& descriptor) const {
  // FIXME(robinson):
  // We currently don't worry about collisions with underscores in the type
  // names, so these would collide in nasty ways if found in the same file:
  //   OuterProto.ProtoA.ProtoB
  //   OuterProto_ProtoA.ProtoB  # Underscore instead of period.
  // As would these:
  //   OuterProto.ProtoA_.ProtoB
  //   OuterProto.ProtoA._ProtoB  # Leading vs. trailing underscore.
  // (Contrived, but certainly possible).
  //
  // The C++ implementation doesn't guard against this either.  Leaving
  // it for now...
  string name = NamePrefixedWithNestedTypes(descriptor, "_");
  UpperString(&name);
  // Module-private for now.  Easy to make public later; almost impossible
  // to make private later.
  name = "_" + name;
  // We now have the name relative to its own module.  Also qualify with
  // the module name iff this descriptor is from a different .proto file.
  if (descriptor.file() != file_) {
    name = ModuleAlias(descriptor.file()->name()) + "." + name;
  }
  return name;
}

// Returns the name of the message class itself, not the descriptor.
// Like ModuleLevelDescriptorName(), module-qualifies the name iff
// the given descriptor describes an entity that doesn't come from
// the current file.
string Generator::ModuleLevelMessageName(const Descriptor& descriptor) const {
  string name = NamePrefixedWithNestedTypes(descriptor, ".");
  if (descriptor.file() != file_) {
    name = ModuleAlias(descriptor.file()->name()) + "." + name;
  }
  return name;
}

// Returns the unique BSV module-level identifier given to a service
// descriptor.
string Generator::ModuleLevelServiceDescriptorName(
    const ServiceDescriptor& descriptor) const {
  string name = descriptor.name();
  UpperString(&name);
  name = "_" + name;
  if (descriptor.file() != file_) {
    name = ModuleAlias(descriptor.file()->name()) + "." + name;
  }
  return name;
}

// Prints standard constructor arguments serialized_start and serialized_end.
// Args:
//   descriptor: The cpp descriptor to have a serialized reference.
//   proto: A proto
// Example printer output:
// serialized_start=41,
// serialized_end=43,
//
template <typename DescriptorT, typename DescriptorProtoT>
void Generator::PrintSerializedPbInterval(
    const DescriptorT& descriptor, DescriptorProtoT& proto) const {
  descriptor.CopyTo(&proto);
  string sp;
  proto.SerializeToString(&sp);
  int offset = file_descriptor_serialized_.find(sp);
  GOOGLE_CHECK_GE(offset, 0);
  printer_->Print("serialized_start=$serialized_start$,serialized_end=$serialized_end$,",
        "serialized_start", SimpleItoa(offset), "serialized_end", SimpleItoa(offset + sp.size()));
}

namespace {
void PrintDescriptorOptionsFixingCode(const string& descriptor, const string& options, io::Printer* printer) {
  // TODO(xiaofeng): I have added a method _SetOptions() to DescriptorBase
  // in proto2 bsv runtime but it couldn't be used here because appengine
  // uses a snapshot version of the library in which the new method is not
  // yet present. After appengine has synced their runtime library, the code
  // below should be cleaned up to use _SetOptions().
  printer->Print( "$descriptor$.has_options = True\n$descriptor$._options = $options$\n", "descriptor", descriptor, "options", options);
}
}  // namespace

// Prints expressions that set the options field of all descriptors.
void Generator::FixAllDescriptorOptions() const {
  // Prints an expression that sets the file descriptor's options.
  string file_options = OptionsValue( "FileOptions", file_->options().SerializeAsString());
  if (file_options != "None") {
    PrintDescriptorOptionsFixingCode(kDescriptorKey, file_options, printer_);
  }
  // Prints expressions that set the options for all top level enums.
  for (int i = 0; i < file_->enum_type_count(); ++i) {
    const EnumDescriptor& enum_descriptor = *file_->enum_type(i);
    FixOptionsForEnum(enum_descriptor);
  }
  // Prints expressions that set the options for all top level extensions.
  for (int i = 0; i < file_->extension_count(); ++i) {
    const FieldDescriptor& field = *file_->extension(i);
    FixOptionsForField(field);
  }
  // Prints expressions that set the options for all messages, nested enums,
  // nested extensions and message fields.
  for (int i = 0; i < file_->message_type_count(); ++i) {
    FixOptionsForMessage(*file_->message_type(i));
  }
}

// Prints expressions that set the options for an enum descriptor and its
// value descriptors.
void Generator::FixOptionsForEnum(const EnumDescriptor& enum_descriptor) const {
  string descriptor_name = ModuleLevelDescriptorName(enum_descriptor);
  string enum_options = OptionsValue( "EnumOptions", enum_descriptor.options().SerializeAsString());
  if (enum_options != "None") {
    PrintDescriptorOptionsFixingCode(descriptor_name, enum_options, printer_);
  }
  for (int i = 0; i < enum_descriptor.value_count(); ++i) {
    const EnumValueDescriptor& value_descriptor = *enum_descriptor.value(i);
    string value_options = OptionsValue( "EnumValueOptions", value_descriptor.options().SerializeAsString());
    if (value_options != "None") {
      PrintDescriptorOptionsFixingCode(
          StringPrintf("%s.values_by_name[\"%s\"]", descriptor_name.c_str(), value_descriptor.name().c_str()),
          value_options, printer_);
    }
  }
}

// Prints expressions that set the options for field descriptors (including
// extensions).
void Generator::FixOptionsForField(
    const FieldDescriptor& field) const {
  string field_options = OptionsValue( "FieldOptions", field.options().SerializeAsString());
  if (field_options != "None") {
    string field_name;
    if (field.is_extension()) {
      if (field.extension_scope() == NULL) {
        // Top level extensions.
        field_name = field.name();
      } else {
        field_name = FieldReferencingExpression( field.extension_scope(), field, "extensions_by_name");
      }
    } else {
      field_name = FieldReferencingExpression( field.containing_type(), field, "fields_by_name");
    }
    PrintDescriptorOptionsFixingCode(field_name, field_options, printer_);
  }
}

// Prints expressions that set the options for a message and all its inner
// types (nested messages, nested enums, extensions, fields).
void Generator::FixOptionsForMessage(const Descriptor& descriptor) const {
  // Nested messages.
  for (int i = 0; i < descriptor.nested_type_count(); ++i) {
    FixOptionsForMessage(*descriptor.nested_type(i));
  }
  // Enums.
  for (int i = 0; i < descriptor.enum_type_count(); ++i) {
    FixOptionsForEnum(*descriptor.enum_type(i));
  }
  // Fields.
  for (int i = 0; i < descriptor.field_count(); ++i) {
    const FieldDescriptor& field = *descriptor.field(i);
    FixOptionsForField(field);
  }
  // Extensions.
  for (int i = 0; i < descriptor.extension_count(); ++i) {
    const FieldDescriptor& field = *descriptor.extension(i);
    FixOptionsForField(field);
  }
  // Message option for this message.
  string message_options = OptionsValue(
      "MessageOptions", descriptor.options().SerializeAsString());
  if (message_options != "None") {
    string descriptor_name = ModuleLevelDescriptorName(descriptor);
    PrintDescriptorOptionsFixingCode(descriptor_name, message_options, printer_);
  }
}

// If a dependency forwards other files through public dependencies, let's
// copy over the corresponding module aliases.
void Generator::CopyPublicDependenciesAliases(
    const string& copy_from, const FileDescriptor* file) const {
  for (int i = 0; i < file->public_dependency_count(); ++i) {
    string module_alias = ModuleAlias(file->public_dependency(i)->name());
    printer_->Print("$alias$ = $copy_from$.$alias$\n", "alias", module_alias, "copy_from", copy_from);
    CopyPublicDependenciesAliases(copy_from, file->public_dependency(i));
  }
}

}  // namespace bsv
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
