#include <torch/csrc/jit/frontend/function_schema_parser.h>
#include <torch/csrc/utils/python_dispatch.h>

#include <ATen/ATen.h>
#include <ATen/TensorSubclassLikeUtils.h>
#include <ATen/core/dispatch/Dispatcher.h>
#include <torch/library.h>

#include <c10/core/SafePyObject.h>
#include <torch/csrc/autograd/python_variable.h>
#include <torch/csrc/jit/python/pybind_utils.h>

#include <pybind11/operators.h>
#include <pybind11/stl.h>
#include <torch/csrc/utils/pybind.h>

#include <iostream>

namespace py = pybind11;

namespace torch {
namespace impl {
namespace dispatch {

torch::Library::Kind parseKind(const std::string& k) {
  static std::unordered_map<std::string, torch::Library::Kind> kind_map = {
      {"DEF", torch::Library::DEF},
      {"IMPL", torch::Library::IMPL},
      {"FRAGMENT", torch::Library::FRAGMENT},
  };
  auto it = kind_map.find(k);
  TORCH_CHECK(it != kind_map.end(), "could not parse ", k);
  return it->second;
}
c10::AliasAnalysisKind parseAliasAnalysisKind(const std::string& k) {
  static std::unordered_map<std::string, c10::AliasAnalysisKind> key_map = {
      {"CONSERVATIVE", c10::AliasAnalysisKind::CONSERVATIVE},
      {"FROM_SCHEMA", c10::AliasAnalysisKind::FROM_SCHEMA},
      {"PURE_FUNCTION", c10::AliasAnalysisKind::PURE_FUNCTION},
      {"", c10::AliasAnalysisKind::FROM_SCHEMA}, // default
  };
  auto it = key_map.find(k);
  TORCH_CHECK(it != key_map.end(), "could not parse ", k);
  return it->second;
}

template <typename Func>
inline torch::CppFunction dispatch_str(const char* key, Func&& raw_f) {
  auto mb_key = std::string(key) == ""
      ? c10::nullopt
      : c10::make_optional(c10::parseDispatchKey(key));
  if (mb_key) {
    return torch::dispatch(*mb_key, std::forward<Func>(raw_f));
  } else {
    torch::CppFunction f(std::forward<Func>(raw_f));
    return f;
  }
}

class PythonKernelHolder : public c10::OperatorKernel {
  c10::SafePyObject func_;

 public:
  PythonKernelHolder(py::object func)
      : func_(func.release().ptr(), getPyInterpreter()) {}

  void operator()(
      const c10::OperatorHandle& op,
      c10::DispatchKeySet keyset,
      torch::jit::Stack* stack) {
    auto arguments = torch::jit::pop(*stack, op.schema().arguments().size());
    py::gil_scoped_acquire g;
    auto args_kwargs = parseIValuesToPyArgsKwargs(op, arguments);
    auto obj = py::reinterpret_steal<py::object>(PyObject_Call(
        func_.ptr(getPyInterpreter()),
        args_kwargs.first.ptr(),
        args_kwargs.second.ptr()));
    if (!obj) {
      throw python_error();
    }
    pushPyOutToStack(op, stack, obj, "PythonKernelHolder");
  }
};

void initDispatchBindings(PyObject* module) {
  auto m = py::handle(module).cast<py::module>();

  py::class_<c10::OperatorHandle>(m, "_DispatchOperatorHandle")
      .def("schema", &c10::OperatorHandle::schema);

  // TODO: figure out how to do chaining
  py::class_<torch::Library>(m, "_DispatchModule")
      .def(
          "def_",
          [](py::object self, const char* schema, const char* alias) {
            self.cast<torch::Library&>().def(
                torch::schema(schema, parseAliasAnalysisKind(alias)));
            return self;
          },
          "",
          py::arg("schema"),
          py::arg("alias") = "")
      // Simulated "legacy" def where alias analysis kind is not set.
      // Ordinarily this can only be exercised from RegisterOperators() API
      // but I am not going to bind that here
      .def(
          "def_legacy",
          [](py::object self, const char* schema) {
            self.cast<torch::Library&>().def(torch::jit::parseSchema(schema));
            return self;
          },
          "",
          py::arg("schema"))
      // We can't conveniently turn Python functions into valid functions
      // in the dispatcher.  So instead we provide a bunch of precanned
      // functions for testing purposes.  You're NOT intended to actually
      // call these functions; they're just here so we can actually register
      // something
      //
      // Mangling scheme: args_rets.  One character per.
      //  t = Tensor
      .def(
          "def_name_t_t",
          [](py::object self,
             const char* name,
             const char* dispatch,
             const char* debug) {
            self.cast<torch::Library&>().def(
                name, dispatch_str(dispatch, [](const at::Tensor& a) {
                        return a;
                      }).debug(debug));
            return self;
          },
          "",
          py::arg("name"),
          py::arg("dispatch") = "",
          py::arg("debug") = "default_def_name_t_t")
      .def(
          "def_schema_t_t",
          [](py::object self,
             const char* schema,
             const char* dispatch,
             const char* alias,
             const char* debug) {
            self.cast<torch::Library&>().def(
                torch::schema(schema, parseAliasAnalysisKind(alias)),
                dispatch_str(dispatch, [](const at::Tensor& a) {
                  return a;
                }).debug(debug));
            return self;
          },
          "",
          py::arg("name"),
          py::arg("dispatch") = "",
          py::arg("alias") = "",
          py::arg("debug") = "default_def_schema_t_t")
      // TODO: maybe consider deduplicating the definitions here, it's getting
      // pretty long
      .def(
          "impl_t_t",
          [](py::object self,
             const char* name,
             const char* dispatch,
             const char* debug) {
            self.cast<torch::Library&>().impl(
                name, dispatch_str(dispatch, [](const at::Tensor& a) {
                        return a;
                      }).debug(debug));
            return self;
          },
          "",
          py::arg("name"),
          py::arg("dispatch") = "",
          py::arg("debug") = "impl_t_t")
      .def(
          "impl_tt_t",
          [](py::object self,
             const char* name,
             const char* dispatch,
             const char* debug) {
            self.cast<torch::Library&>().impl(
                name,
                dispatch_str(
                    dispatch,
                    [](const at::Tensor& a, const at::Tensor& b) { return a; })
                    .debug(debug));
            return self;
          },
          "",
          py::arg("name"),
          py::arg("dispatch") = "",
          py::arg("debug") = "")
      .def(
          "impl",
          [](py::object self,
             const char* name,
             const char* dispatch,
             py::object func) {
            HANDLE_TH_ERRORS
            self.cast<torch::Library&>().impl(
                name,
                dispatch_str(
                    dispatch,
                    CppFunction::makeFromBoxedFunctor(
                        std::make_unique<PythonKernelHolder>(
                            std::move(func)))));
            END_HANDLE_TH_ERRORS_PYBIND
          },
          "",
          py::arg("name"),
          py::arg("dispatch"),
          py::arg("func"))
      .def(
          "define",
          [](py::object self, const char* schema, const char* alias_analysis) {
            self.cast<torch::Library&>().def(
                torch::schema(schema, parseAliasAnalysisKind(alias_analysis)));
            return torch::schema(schema, parseAliasAnalysisKind(alias_analysis))
                .name();
          },
          "",
          py::arg("schema"),
          py::arg("alias_analysis") = "")
      .def(
          "fallback_fallthrough",
          [](py::object self, const char* dispatch) {
            self.cast<torch::Library&>().fallback(
                dispatch_str(dispatch, CppFunction::makeFallthrough()));
            return self;
          },
          "",
          py::arg("dispatch") = "");

  m.def(
      "_dispatch_library",
      [](const char* kind,
         std::string name,
         const char* dispatch,
         const char* file,
         uint32_t linenum) {
        HANDLE_TH_ERRORS
        return std::make_unique<torch::Library>(
            parseKind(kind),
            std::move(name),
            std::string(dispatch) == ""
                ? c10::nullopt
                : c10::make_optional(c10::parseDispatchKey(dispatch)),
            "/dev/null", // temporary workaround
            linenum);
        END_HANDLE_TH_ERRORS_PYBIND
      },
      "",
      py::arg("kind"),
      py::arg("name"),
      py::arg("dispatch"),
      py::arg("file") = "/dev/null",
      py::arg("linenum") = 0);

  m.def("_dispatch_dump", [](const char* name) -> std::string {
    auto op = c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
    if (!op) {
      return "";
    } else {
      return op->dumpState();
    }
  });

  m.def("_dispatch_dump_table", [](const char* name) -> std::string {
    auto op = c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
    if (!op) {
      return "";
    } else {
      return op->dumpComputedTable();
    }
  });

  m.def("_dispatch_check_invariants", [](const char* name) {
    auto op = c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
    if (!op) {
    } else {
      return op->checkInvariants();
    }
  });

  m.def("_dispatch_check_all_invariants", []() {
    c10::Dispatcher::singleton().checkInvariants();
  });

  m.def("_dispatch_has_kernel", [](const char* name) -> bool {
    auto op = c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
    return static_cast<bool>(op);
  });

  m.def(
      // Returns whether or not a direct kernel registration exists
      // for this <op_name, dispatch_key> pair.
      "_dispatch_has_kernel_for_dispatch_key",
      [](const char* name, const char* dispatch) -> bool {
        auto op =
            c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
        TORCH_CHECK(op, "operator ", name, " does not exist");
        return op->hasKernelForDispatchKey(c10::parseDispatchKey(dispatch));
      });

  m.def(
      // Returns whether or not there is an entry in the runtime computed
      // dispatch table, for this <op_name, dispatch_key> pair. For example, if
      // "op" has a `CompositeImplicitAutograd` kernel, Then
      // _dispatch_has_computed_kernel_for_dispatch_key(op, backend) will return
      // true for all backends that are part of the alias set for
      // CompositeImplicitAutograd.
      "_dispatch_has_computed_kernel_for_dispatch_key",
      [](const char* name, const char* dispatch) -> bool {
        auto op =
            c10::Dispatcher::singleton().findOp(torch::jit::parseName(name));
        TORCH_CHECK(op, "operator ", name, " does not exist");
        return op->hasComputedKernelForDispatchKey(
            c10::parseDispatchKey(dispatch));
      });

  m.def("_dispatch_find_dangling_impls", []() -> std::vector<std::string> {
    auto danglingImpls = c10::Dispatcher::singleton().findDanglingImpls();

    std::vector<std::string> states;
    states.reserve(danglingImpls.size());
    for (auto& danglingImpl : danglingImpls) {
      states.push_back(danglingImpl.dumpState());
    }

    return states;
  });

  m.def(
      "_dispatch_tls_set_dispatch_key_excluded",
      [](const char* dispatch_key, bool desired_state) {
        c10::impl::tls_set_dispatch_key_excluded(
            c10::parseDispatchKey(dispatch_key), desired_state);
      });
  m.def("_dispatch_tls_is_dispatch_key_excluded", [](const char* dispatch_key) {
    return c10::impl::tls_is_dispatch_key_excluded(
        c10::parseDispatchKey(dispatch_key));
  });

  m.def("_dispatch_isTensorSubclassLike", [](const at::Tensor& tensor) {
    return at::isTensorSubclassLike(tensor);
  });

  m.def("_dispatch_key_name", [](uint64_t dispatch_key) {
    auto dt = (c10::DispatchKey)dispatch_key;
    return c10::toString(dt);
  });
  m.def("_dispatch_num_backends", []() { return c10::num_backends; });

  py::enum_<c10::DispatchKey>(m, "DispatchKey")
      .value("Undefined", c10::DispatchKey::Undefined)
      .value("Dense", c10::DispatchKey::Dense)
      .value("BackendSelect", c10::DispatchKey::BackendSelect)
      .value("CPU", c10::DispatchKey::CPU)
      .value("CUDA", c10::DispatchKey::CUDA)
      .value("AutocastCPU", c10::DispatchKey::AutocastCPU)
      .value("AutocastCUDA", c10::DispatchKey::AutocastCUDA)
      .value("AutogradCPU", c10::DispatchKey::AutogradCPU)
      .value("ADInplaceOrView", c10::DispatchKey::ADInplaceOrView)
      .value("AutogradCUDA", c10::DispatchKey::AutogradCUDA)
      .value("PythonTLSSnapshot", c10::DispatchKey::PythonTLSSnapshot)
      .value("Python", c10::DispatchKey::Python);

  py::class_<c10::DispatchKeySet>(m, "DispatchKeySet")
      .def(py::init<c10::DispatchKey>())
      .def("__or__", &c10::DispatchKeySet::operator|)
      .def("__sub__", &c10::DispatchKeySet::operator-)
      .def("__and__", &c10::DispatchKeySet::operator&)
      .def("highestPriorityTypeId", &c10::DispatchKeySet::highestPriorityTypeId)
      .def("has", &c10::DispatchKeySet::has);

  m.def("_dispatch_keyset_full_after", [](c10::DispatchKey t) {
    return c10::DispatchKeySet(c10::DispatchKeySet::FULL_AFTER, t);
  });

  m.def("_dispatch_keyset_to_string", [](c10::DispatchKeySet keyset) {
    return c10::toString(keyset);
  });

  m.def("_dispatch_keys", [](const at::Tensor& tensor) {
    auto* impl = tensor.unsafeGetTensorImpl();
    return impl->key_set();
  });
  m.def("_dispatch_tls_local_include_set", []() {
    return c10::impl::tls_local_dispatch_key_set().included_;
  });
  m.def("_dispatch_tls_local_exclude_set", []() {
    return c10::impl::tls_local_dispatch_key_set().excluded_;
  });
  py::class_<c10::impl::ExcludeDispatchKeyGuard>(m, "ExcludeDispatchKeyGuard")
      .def(py::init<c10::DispatchKeySet>());

  py::class_<at::AutoDispatchBelowAutograd>(m, "_AutoDispatchBelowAutograd")
      .def(py::init<>());

  // Prints out the name of every operator that has a kernel registered to the
  // Dispatcher under [dispatch_key]. If no arguments are specified, it'll print
  // out the name of every operator that the Dispatcher knows of. This can be
  // useful to answer questions like "list all operators that do not have a CPU
  // kernel".
  m.def(
      "_dispatch_print_registrations_for_dispatch_key",
      [](const char* dispatch_key = "") {
        auto k = std::string(dispatch_key) == ""
            ? c10::nullopt
            : c10::make_optional(c10::parseDispatchKey(dispatch_key));
        auto op_names =
            c10::Dispatcher::singleton().getRegistrationsForDispatchKey(k);
        for (auto& op : op_names) {
          std::cout << op << std::endl;
        }
      },
      py::arg("dispatch_key") = static_cast<const char*>(""));

  m.def(
      "_dispatch_get_registrations_for_dispatch_key",
      [](const char* dispatch_key = "") {
        auto k = std::string(dispatch_key) == ""
            ? c10::nullopt
            : c10::make_optional(c10::parseDispatchKey(dispatch_key));
        auto op_names =
            c10::Dispatcher::singleton().getRegistrationsForDispatchKey(k);
        std::vector<std::string> names;
        names.reserve(op_names.size());
        for (auto& op : op_names) {
          names.push_back(
              op.name + (op.overload_name == "" ? "" : "." + op.overload_name));
        }
        return names;
      },
      py::arg("dispatch_key") = static_cast<const char*>(""));
}

} // namespace dispatch
} // namespace impl
} // namespace torch
