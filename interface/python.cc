/*
 * Copyright 2011,2015 Sven Verdoolaege. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY SVEN VERDOOLAEGE ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SVEN VERDOOLAEGE OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as
 * representing official policies, either expressed or implied, of
 * Sven Verdoolaege.
 */ 

#include "isl_config.h"

#include <stdio.h>
#include <iostream>
#include <map>
#include <vector>
#include <clang/AST/Attr.h>
#include "extract_interface.h"
#include "python.h"

/* Return a sequence of the types of which the given type declaration is
 * marked as being a subtype.
 */
static vector<string> find_superclasses(RecordDecl *decl)
{
	vector<string> super;

	if (!decl->hasAttrs())
		return super;

	string sub = "isl_subclass";
	size_t len = sub.length();
	AttrVec attrs = decl->getAttrs();
	for (AttrVec::const_iterator i = attrs.begin() ; i != attrs.end(); ++i) {
		const AnnotateAttr *ann = dyn_cast<AnnotateAttr>(*i);
		if (!ann)
			continue;
		string s = ann->getAnnotation().str();
		if (s.substr(0, len) == sub) {
			s = s.substr(len + 1, s.length() - len  - 2);
			super.push_back(s);
		}
	}

	return super;
}

/* Is decl marked as a constructor?
 */
static bool is_constructor(Decl *decl)
{
	return has_annotation(decl, "isl_constructor");
}

/* Is decl marked as consuming a reference?
 */
static bool takes(Decl *decl)
{
	return has_annotation(decl, "isl_take");
}

/* isl_class collects all constructors and methods for an isl "class".
 * "name" is the name of the class.
 * "type" is the declaration that introduces the type.
 */
struct isl_class {
	string name;
	RecordDecl *type;
	set<FunctionDecl *> constructors;
	set<FunctionDecl *> methods;

	bool is_static(FunctionDecl *method);

	void print(map<string, isl_class> &classes, set<string> &done);
	void print_constructor(FunctionDecl *method);
	void print_method(FunctionDecl *method, vector<string> super);
};

/* Return the class that has a name that matches the initial part
 * of the name of function "fd" or NULL if no such class could be found.
 */
static isl_class *method2class(map<string, isl_class> &classes,
	FunctionDecl *fd)
{
	string best;
	map<string, isl_class>::iterator ci;
	string name = fd->getNameAsString();

	for (ci = classes.begin(); ci != classes.end(); ++ci) {
		if (name.substr(0, ci->first.length()) == ci->first)
			best = ci->first;
	}

	if (classes.find(best) == classes.end()) {
		cerr << "Unable to find class of " << name << endl;
		return NULL;
	}

	return &classes[best];
}

/* Is "type" the type "isl_ctx *"?
 */
static bool is_isl_ctx(QualType type)
{
	if (!type->isPointerType())
		return 0;
	type = type->getPointeeType();
	if (type.getAsString() != "isl_ctx")
		return false;

	return true;
}

/* Is the first argument of "fd" of type "isl_ctx *"?
 */
static bool first_arg_is_isl_ctx(FunctionDecl *fd)
{
	ParmVarDecl *param;

	if (fd->getNumParams() < 1)
		return false;

	param = fd->getParamDecl(0);
	return is_isl_ctx(param->getOriginalType());
}

/* Is "type" that of a pointer to an isl_* structure?
 */
static bool is_isl_type(QualType type)
{
	if (type->isPointerType()) {
		string s;

		type = type->getPointeeType();
		if (type->isFunctionType())
			return false;
		s = type.getAsString();
		return s.substr(0, 4) == "isl_";
	}

	return false;
}

/* Is "type" the type isl_bool?
 */
static bool is_isl_bool(QualType type)
{
	string s;

	if (type->isPointerType())
		return false;

	s = type.getAsString();
	return s == "isl_bool";
}

/* Is "type" that of a pointer to a function?
 */
static bool is_callback(QualType type)
{
	if (!type->isPointerType())
		return false;
	type = type->getPointeeType();
	return type->isFunctionType();
}

/* Is "type" that of "char *" of "const char *"?
 */
static bool is_string(QualType type)
{
	if (type->isPointerType()) {
		string s = type->getPointeeType().getAsString();
		return s == "const char" || s == "char";
	}

	return false;
}

/* Return the name of the type that "type" points to.
 * The input "type" is assumed to be a pointer type.
 */
static string extract_type(QualType type)
{
	if (type->isPointerType())
		return type->getPointeeType().getAsString();
	assert(0);
}

/* Drop the "isl_" initial part of the type name "name".
 */
static string type2python(string name)
{
	return name.substr(4);
}

/* Should "method" be considered to be a static method?
 * That is, is the first argument something other than
 * an instance of the class?
 */
bool isl_class::is_static(FunctionDecl *method)
{
	ParmVarDecl *param = method->getParamDecl(0);
	QualType type = param->getOriginalType();

	if (!is_isl_type(type))
		return true;
	return extract_type(type) != name;
}

/* Print the header of the method "name" with "n_arg" arguments.
 * If "is_static" is set, then mark the python method as static.
 */
static void print_method_header(bool is_static, const string &name, int n_arg)
{
	if (is_static)
		printf("    @staticmethod\n");

	printf("    def %s(arg0", name.c_str());
	for (int i = 1; i < n_arg; ++i)
		printf(", arg%d", i);
	printf("):\n");
}

/* Construct a wrapper for a callback argument (at position "arg").
 * Assign the wrapper to "cb".  We assume here that a function call
 * has at most one callback argument.
 *
 * The wrapper converts the arguments of the callback to python types.
 * If any exception is thrown, the wrapper keeps track of it in exc_info[0]
 * and returns -1.  Otherwise the wrapper returns 0.
 */
static void print_callback(QualType type, int arg)
{
	const FunctionProtoType *fn = type->getAs<FunctionProtoType>();
	unsigned n_arg = fn->getNumArgs();

	printf("        exc_info = [None]\n");
	printf("        fn = CFUNCTYPE(c_int");
	for (int i = 0; i < n_arg - 1; ++i) {
		QualType arg_type = fn->getArgType(i);
		assert(is_isl_type(arg_type));
		printf(", c_void_p");
	}
	printf(", c_void_p)\n");
	printf("        def cb_func(");
	for (int i = 0; i < n_arg; ++i) {
		if (i)
			printf(", ");
		printf("cb_arg%d", i);
	}
	printf("):\n");
	for (int i = 0; i < n_arg - 1; ++i) {
		string arg_type;
		arg_type = type2python(extract_type(fn->getArgType(i)));
		printf("            cb_arg%d = %s(ctx=arg0.ctx, "
			"ptr=cb_arg%d)\n", i, arg_type.c_str(), i);
	}
	printf("            try:\n");
	printf("                arg%d(", arg);
	for (int i = 0; i < n_arg - 1; ++i) {
		if (i)
			printf(", ");
		printf("cb_arg%d", i);
	}
	printf(")\n");
	printf("            except:\n");
	printf("                import sys\n");
	printf("                exc_info[0] = sys.exc_info()\n");
	printf("                return -1\n");
	printf("            return 0\n");
	printf("        cb = fn(cb_func)\n");
}

/* Print the argument at position "arg" in call to "fd".
 *
 * If the argument is a callback, then print a reference to
 * the callback wrapper "cb".
 * Otherwise, if the argument is marked as consuming a reference,
 * then pass a copy of the the pointer stored in the corresponding
 * argument passed to the Python method.
 * Otherwise, pass this pointer itself.
 */
static void print_arg_in_call(FunctionDecl *fd, int arg)
{
	ParmVarDecl *param = fd->getParamDecl(arg);
	QualType type = param->getOriginalType();
	if (is_callback(type)) {
		printf("cb");
	} else if (takes(param)) {
		string type_s = extract_type(type);
		printf("isl.%s_copy(arg%d.ptr)", type_s.c_str(), arg);
	} else {
		printf("arg%d.ptr", arg);
	}
}

/* Print a python method corresponding to the C function "method".
 * "super" contains the superclasses of the class to which the method belongs.
 *
 * If the first argument of "method" is something other than an instance
 * of the class, then mark the python method as static.
 *
 * If the function has a callback argument, then it also has a "user"
 * argument.  Since Python has closures, there is no need for such
 * a user argument in the Python interface, so we simply drop it.
 * We also create a wrapper ("cb") for the callback.
 *
 * For each argument of the function that refers to an isl structure,
 * including the object on which the method is called,
 * we check if the corresponding actual argument is of the right type.
 * If not, we try to convert it to the right type.
 * It that doesn't work and if subclass is set, we try to convert self
 * to the type of the first superclass in "super" and
 * call the corresponding method.
 *
 * If the function consumes a reference, then we pass it a copy of
 * the actual argument.
 *
 * If the return type is isl_bool, then convert the result to
 * a Python boolean, raising an error on isl_bool_error.
 */
void isl_class::print_method(FunctionDecl *method, vector<string> super)
{
	string fullname = method->getName();
	string cname = fullname.substr(name.length() + 1);
	int num_params = method->getNumParams();
	int drop_user = 0;

	for (int i = 1; i < num_params; ++i) {
		ParmVarDecl *param = method->getParamDecl(i);
		QualType type = param->getOriginalType();
		if (is_callback(type))
			drop_user = 1;
	}

	print_method_header(is_static(method), cname, num_params - drop_user);

	for (int i = 0; i < num_params; ++i) {
		ParmVarDecl *param = method->getParamDecl(i);
		string type;
		if (!is_isl_type(param->getOriginalType()))
			continue;
		type = type2python(extract_type(param->getOriginalType()));
		printf("        try:\n");
		printf("            if not arg%d.__class__ is %s:\n",
			i, type.c_str());
		printf("                arg%d = %s(arg%d)\n",
			i, type.c_str(), i);
		printf("        except:\n");
		if (i > 0 && super.size() > 0) {
			printf("            return %s(arg0).%s(",
				type2python(super[0]).c_str(), cname.c_str());
			for (int i = 1; i < num_params - drop_user; ++i) {
				if (i != 1)
					printf(", ");
				printf("arg%d", i);
			}
			printf(")\n");
		} else
			printf("            raise\n");
	}
	for (int i = 1; i < num_params; ++i) {
		ParmVarDecl *param = method->getParamDecl(i);
		QualType type = param->getOriginalType();
		if (!is_callback(type))
			continue;
		print_callback(type->getPointeeType(), i);
	}
	printf("        res = isl.%s(", fullname.c_str());
	print_arg_in_call(method, 0);
	for (int i = 1; i < num_params - drop_user; ++i) {
		printf(", ");
		print_arg_in_call(method, i);
	}
	if (drop_user)
		printf(", None");
	printf(")\n");

	if (is_isl_type(method->getReturnType())) {
		string type;
		type = type2python(extract_type(method->getReturnType()));
		printf("        return %s(ctx=arg0.ctx, ptr=res)\n",
			type.c_str());
	} else {
		if (drop_user) {
			printf("        if exc_info[0] != None:\n");
			printf("            raise exc_info[0][0], "
				"exc_info[0][1], exc_info[0][2]\n");
		}
		if (is_isl_bool(method->getReturnType())) {
			printf("        if res < 0:\n");
			printf("            raise\n");
			printf("        return bool(res)\n");
		} else {
			printf("        return res\n");
		}
	}
}

/* Print part of the constructor for this isl_class.
 *
 * In particular, check if the actual arguments correspond to the
 * formal arguments of "cons" and if so call "cons" and put the
 * result in self.ptr and a reference to the default context in self.ctx.
 *
 * If the function consumes a reference, then we pass it a copy of
 * the actual argument.
 */
void isl_class::print_constructor(FunctionDecl *cons)
{
	string fullname = cons->getName();
	string cname = fullname.substr(name.length() + 1);
	int num_params = cons->getNumParams();
	int drop_ctx = first_arg_is_isl_ctx(cons);

	printf("        if len(args) == %d", num_params - drop_ctx);
	for (int i = drop_ctx; i < num_params; ++i) {
		ParmVarDecl *param = cons->getParamDecl(i);
		if (is_isl_type(param->getOriginalType())) {
			string type;
			type = extract_type(param->getOriginalType());
			type = type2python(type);
			printf(" and args[%d].__class__ is %s",
				i - drop_ctx, type.c_str());
		} else
			printf(" and type(args[%d]) == str", i - drop_ctx);
	}
	printf(":\n");
	printf("            self.ctx = Context.getDefaultInstance()\n");
	printf("            self.ptr = isl.%s(", fullname.c_str());
	if (drop_ctx)
		printf("self.ctx");
	for (int i = drop_ctx; i < num_params; ++i) {
		ParmVarDecl *param = cons->getParamDecl(i);
		if (i)
			printf(", ");
		if (is_isl_type(param->getOriginalType())) {
			if (takes(param)) {
				string type;
				type = extract_type(param->getOriginalType());
				printf("isl.%s_copy(args[%d].ptr)",
					type.c_str(), i - drop_ctx);
			} else
				printf("args[%d].ptr", i - drop_ctx);
		} else
			printf("args[%d]", i - drop_ctx);
	}
	printf(")\n");
	printf("            return\n");
}

/* Print the header of the class "name" with superclasses "super".
 */
static void print_class_header(const string &name, const vector<string> &super)
{
	printf("class %s", name.c_str());
	if (super.size() > 0) {
		printf("(");
		for (int i = 0; i < super.size(); ++i) {
			if (i > 0)
				printf(", ");
			printf("%s", type2python(super[i]).c_str());
		}
		printf(")");
	}
	printf(":\n");
}

/* Tell ctypes about the return type of "fd".
 * In particular, if "fd" returns a pointer to an isl object,
 * then tell ctypes it returns a "c_void_p".
 * Similarly, if "fd" returns an isl_bool,
 * then tell ctypes it returns a "c_bool".
 */
static void print_restype(FunctionDecl *fd)
{
	string fullname = fd->getName();
	QualType type = fd->getReturnType();
	if (is_isl_type(type))
		printf("isl.%s.restype = c_void_p\n", fullname.c_str());
	else if (is_isl_bool(type))
		printf("isl.%s.restype = c_bool\n", fullname.c_str());
}

/* Tell ctypes about the types of the arguments of the constructor "fd".
 */
static void print_argtypes(FunctionDecl *fd)
{
	string fullname = fd->getName();
	printf("isl.%s.argtypes = [", fullname.c_str());
	for (int i = 0; i < fd->getNumParams(); ++i) {
		ParmVarDecl *param = fd->getParamDecl(i);
		QualType type = param->getOriginalType();
		if (i)
			printf(", ");
		if (is_isl_ctx(type))
			printf("Context");
		else if (is_isl_type(type))
			printf("c_void_p");
		else if (is_string(type))
			printf("c_char_p");
		else
			printf("c_int");
	}
	printf("]\n");
}

/* Print out the definition of this isl_class.
 *
 * We first check if this isl_class is a subclass of one or more other classes.
 * If it is, we make sure those superclasses are printed out first.
 *
 * Then we print a constructor with several cases, one for constructing
 * a Python object from a return value and one for each function that
 * was marked as a constructor.
 *
 * Next, we print out some common methods and the methods corresponding
 * to functions that are not marked as constructors.
 *
 * Finally, we tell ctypes about the types of the arguments of the
 * constructor functions and the return types of those function returning
 * an isl object.
 */
void isl_class::print(map<string, isl_class> &classes, set<string> &done)
{
	string p_name = type2python(name);
	set<FunctionDecl *>::iterator in;
	vector<string> super = find_superclasses(type);

	for (int i = 0; i < super.size(); ++i)
		if (done.find(super[i]) == done.end())
			classes[super[i]].print(classes, done);
	done.insert(name);

	printf("\n");
	print_class_header(p_name, super);
	printf("    def __init__(self, *args, **keywords):\n");

	printf("        if \"ptr\" in keywords:\n");
	printf("            self.ctx = keywords[\"ctx\"]\n");
	printf("            self.ptr = keywords[\"ptr\"]\n");
	printf("            return\n");

	for (in = constructors.begin(); in != constructors.end(); ++in)
		print_constructor(*in);
	printf("        raise Error\n");
	printf("    def __del__(self):\n");
	printf("        if hasattr(self, 'ptr'):\n");
	printf("            isl.%s_free(self.ptr)\n", name.c_str());
	printf("    def __str__(self):\n");
	printf("        ptr = isl.%s_to_str(self.ptr)\n", name.c_str());
	printf("        res = str(cast(ptr, c_char_p).value)\n");
	printf("        libc.free(ptr)\n");
	printf("        return res\n");
	printf("    def __repr__(self):\n");
	printf("        return 'isl.%s(\"%%s\")' %% str(self)\n",
		p_name.c_str());

	for (in = methods.begin(); in != methods.end(); ++in)
		print_method(*in, super);

	printf("\n");
	for (in = constructors.begin(); in != constructors.end(); ++in) {
		print_restype(*in);
		print_argtypes(*in);
	}
	for (in = methods.begin(); in != methods.end(); ++in)
		print_restype(*in);
	printf("isl.%s_free.argtypes = [c_void_p]\n", name.c_str());
	printf("isl.%s_to_str.argtypes = [c_void_p]\n", name.c_str());
	printf("isl.%s_to_str.restype = POINTER(c_char)\n", name.c_str());
}

/* Generate a python interface based on the extracted types and functions.
 * We first collect all functions that belong to a certain type,
 * separating constructors from regular methods.
 *
 * Then we print out each class in turn.  If one of these is a subclass
 * of some other class, it will make sure the superclass is printed out first.
 */
void generate_python(set<RecordDecl *> &types, set<FunctionDecl *> functions)
{
	map<string, isl_class> classes;
	map<string, isl_class>::iterator ci;
	set<string> done;

	set<RecordDecl *>::iterator it;
	for (it = types.begin(); it != types.end(); ++it) {
		RecordDecl *decl = *it;
		string name = decl->getName();
		classes[name].name = name;
		classes[name].type = decl;
	}

	set<FunctionDecl *>::iterator in;
	for (in = functions.begin(); in != functions.end(); ++in) {
		isl_class *c = method2class(classes, *in);
		if (!c)
			continue;
		if (is_constructor(*in))
			c->constructors.insert(*in);
		else
			c->methods.insert(*in);
	}

	for (ci = classes.begin(); ci != classes.end(); ++ci) {
		if (done.find(ci->first) == done.end())
			ci->second.print(classes, done);
	}
}
