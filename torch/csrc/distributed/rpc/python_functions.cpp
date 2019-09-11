#include <torch/csrc/distributed/rpc/python_functions.h>

#include <torch/csrc/distributed/rpc/message.h>
#include <torch/csrc/distributed/rpc/python_remote_call.h>
#include <torch/csrc/distributed/rpc/python_rpc_handler.h>
#include <torch/csrc/distributed/rpc/rref.h>
#include <torch/csrc/distributed/rpc/rref_context.h>
#include <torch/csrc/distributed/rpc/rref_proto.h>
#include <torch/csrc/distributed/rpc/script_call.h>
#include <torch/csrc/distributed/rpc/script_remote_call.h>
#include <torch/csrc/distributed/rpc/script_ret.h>

#include <torch/csrc/jit/pybind_utils.h>

namespace torch {
namespace distributed {
namespace rpc {

namespace {

std::shared_ptr<Operator> matchBuiltinOp(
    const std::string& opName,
    const py::args& args,
    const py::kwargs& kwargs,
    Stack& stack) {
  Symbol symbol = Symbol::fromQualString(opName);
  if (symbol.is_aten()) {
    for (const auto& op : torch::jit::getAllOperatorsFor(symbol)) {
      try {
        // FIXME: This is temporary solution. We should at least refactor
        // ``createStackForSchema`` to avoid throwing an error.
        stack = torch::jit::createStackForSchema(
            op->schema(), args, kwargs, c10::nullopt);

      } catch (std::runtime_error& e) {
        VLOG(1) << "Couldn't match schema: " << op->schema()
                << " to args: " << args << " and kwargs: " << kwargs
                << ", reason: " << e.what();
        continue;
      }

      // Found the right op!
      return op;
    }
  }

  AT_ERROR(
      "Failed to match operator name ",
      opName,
      " and arguments "
      "(args: ",
      args,
      ", kwargs: ",
      kwargs,
      ") to a builtin operator");

  // builtin operators.
}

void finishAcceptUserRRef(const Message& message) {
  RRefContext::handleException(message);
  RemoteRet rr = RemoteRet::fromMessage(message);
  auto& ctx = RRefContext::getInstance();
  TORCH_INTERNAL_ASSERT(ctx->getWorkerId() == rr.owner_,
      "Worker ",
      ctx->getWorkerId(),
      " received a RREF_USER_ACCEPT message of a different owner ",
      rr.owner_);
  ctx->finishUserRRef(rr.rrefId_, rr.forkId_);
}

} // namespace

py::object toPyObj(const Message& message) {
  switch (message.type()) {
    case MessageType::SCRIPT_RET: {
      ScriptRet ret = ScriptRet::fromMessage(message);
      Stack stack;
      stack.push_back(ret.value());
      AutoGIL ag;
      return torch::jit::createPyObjectForStack(std::move(stack));
    }
    case MessageType::PYTHON_RET: {
      return PythonRpcHandler::loadPythonUDFResult(message);
    }
    case MessageType::EXCEPTION: {
      std::string err(message.payload().begin(), message.payload().end());
      throw std::runtime_error(err);
    }
    default: {
      AT_ERROR("Unrecognized response message type ", message.type());
    }
  }
}

std::shared_ptr<FutureMessage> pyRpcBuiltin(
    RpcAgent& agent,
    const WorkerId& dst,
    const std::string& opName,
    const py::args& args,
    const py::kwargs& kwargs) {
  Stack stack;
  auto op = matchBuiltinOp(opName, args, kwargs, stack);
  return agent.send(dst, ScriptCall(op, std::move(stack)).toMessage());
}

PyRRef pyRemoteBuiltin(
    RpcAgent& agent,
    const WorkerId& dst,
    const std::string& opName,
    const py::args& args,
    const py::kwargs& kwargs) {
  Stack stack;
  auto op = matchBuiltinOp(opName, args, kwargs, stack);

  auto& ctx = RRefContext::getInstance();
  TORCH_INTERNAL_ASSERT(ctx->getWorkerId() != dst.id_,
      "Does not support creating RRef on self yet.");
  auto userRRef = ctx->createUserRRef<IValue>(dst.id_);
  auto fm = agent.send(
      dst,
      ScriptRemoteCall(
          op,
          std::move(stack),
          userRRef->id().toIValue(),
          userRRef->forkId().toIValue())
          .toMessage());
  fm->addCallback(finishAcceptUserRRef);
  return PyRRef(userRRef);
}

std::shared_ptr<FutureMessage> pyRpcPythonUdf(
    RpcAgent& agent,
    const WorkerId& dst,
    const std::string& pickledPythonUDF) {
  std::vector<char> data(pickledPythonUDF.begin(), pickledPythonUDF.end());
  std::vector<torch::Tensor> tensor_table;

  return agent.send(
      dst,
      Message(
          std::move(data), std::move(tensor_table), MessageType::PYTHON_CALL));
}

PyRRef pyRemotePythonUdf(
    RpcAgent& agent,
    const WorkerId& dst,
    const std::string& pickledPythonUDF) {
  auto& ctx = RRefContext::getInstance();
  TORCH_INTERNAL_ASSERT(ctx->getWorkerId() != dst.id_,
      "Does not support creating RRef on self yet.");
  auto userRRef = ctx->createUserRRef<py::object>(dst.id_);
  auto fm = agent.send(
      dst,
      PythonRemoteCall(
          pickledPythonUDF,
          userRRef->id().toIValue(),
          userRRef->forkId().toIValue())
          .toMessage());

  fm->addCallback(finishAcceptUserRRef);
  return PyRRef(userRRef);
}

} // namespace rpc
} // namespace distributed
} // namespace torch
