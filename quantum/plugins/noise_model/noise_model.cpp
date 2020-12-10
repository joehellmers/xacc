#include "NoiseModel.hpp"
#include "xacc_plugin.hpp"
#include "json.hpp"
#include "xacc.hpp"
#include <Eigen/Dense>
#include "CommonGates.hpp"

namespace {
constexpr double NUM_TOL = 1e-9;
bool allClose(const Eigen::MatrixXcd &in_mat1, const Eigen::MatrixXcd &in_mat2,
              double in_tol = NUM_TOL) {
  if (in_mat1.rows() == in_mat2.rows() && in_mat1.cols() == in_mat2.cols()) {
    for (int i = 0; i < in_mat1.rows(); ++i) {
      for (int j = 0; j < in_mat1.cols(); ++j) {
        if (std::abs(in_mat1(i, j) - in_mat2(i, j)) > in_tol) {
          return false;
        }
      }
    }

    return true;
  }
  return false;
}

bool isIdentity(const Eigen::MatrixXcd &mat, double threshold = NUM_TOL) {
  Eigen::MatrixXcd idMat = Eigen::MatrixXcd::Identity(mat.rows(), mat.cols());
  return allClose(mat, idMat, threshold);
}

bool validateKrausCPTP(const std::vector<Eigen::MatrixXcd> &mats,
                       double threshold = NUM_TOL) {
  if (mats.empty()) {
    return true;
  }
  Eigen::MatrixXcd cptp =
      Eigen::MatrixXcd::Zero(mats[0].rows(), mats[0].cols());
  for (const auto &mat : mats) {
    cptp = cptp + mat.adjoint() * mat;
  }
  return isIdentity(cptp, threshold);
}

using cMat = std::vector<std::vector<std::complex<double>>>;
using cMatPair = std::vector<std::vector<std::pair<double, double>>>;

cMat convertToStdMat(const Eigen::MatrixXcd &in_eigenMat) {
  const size_t dim = in_eigenMat.rows();
  cMat result;
  for (size_t row = 0; row < dim; ++row) {
    std::vector<std::complex<double>> rowVec;
    for (size_t col = 0; col < dim; ++col) {
      rowVec.emplace_back(in_eigenMat(row, col));
    }
    result.emplace_back(rowVec);
  }
  return result;
}

cMatPair convertToMatOfPairs(const cMat &in_mat) {
  cMatPair result;
  const size_t dim = in_mat.size();
  for (size_t row = 0; row < dim; ++row) {
    std::vector<std::pair<double, double>> rowVec;
    const auto &cRowVec = in_mat[row];
    for (size_t col = 0; col < dim; ++col) {
      const auto elem = cRowVec[col];
      rowVec.emplace_back(elem.real(), elem.imag());
    }
    result.emplace_back(rowVec);
  }
  return result;
}
} // namespace

namespace xacc {
class JsonNoiseModel : public NoiseModel {
public:
  enum class BitOrder { MSB, LSB };

  struct KrausChannel {
    std::vector<std::string> bit_locs;
    std::vector<Eigen::MatrixXcd> matrix;
  };

  // Identifiable interface impls
  const std::string name() const override { return "json"; }
  const std::string description() const override {
    return "Noise model whose noise channel Kraus operators are provided in a "
           "JSON input file.";
  }

  virtual void initialize(const HeterogeneousMap &params) override {
    if (params.stringExists("noise-model")) {
      std::string noise_model_str = params.getString("noise-model");
      // Check if this is a file name
      std::ifstream test(noise_model_str);
      if (test) {
        std::string str((std::istreambuf_iterator<char>(test)),
                        std::istreambuf_iterator<char>());
        m_noiseModel = nlohmann::json::parse(str);
      } else {
        m_noiseModel = nlohmann::json::parse(params.getString("noise-model"));
      }
    }

    // Debug:
    std::cout << "HOWDY: \n" << m_noiseModel.dump() << "\n";
    const auto bit_order = m_noiseModel["bit_order"].get<std::string>();
    if (bit_order == "MSB") {
      m_bitOrder = BitOrder::MSB;
    } else if (bit_order == "LSB") {
      m_bitOrder = BitOrder::LSB;
    } else {
      xacc::error("Unknown value: " + bit_order);
    }

    auto gate_noise = m_noiseModel["gate_noise"];
    for (auto iter = gate_noise.begin(); iter != gate_noise.end(); ++iter) {
      auto noise_info = *iter;
      const auto gate_name = noise_info["gate_name"].get<std::string>();
      const auto register_location =
          noise_info["register_location"].get<std::vector<std::string>>();
      const auto gateKey = createGateLookupKey(gate_name, register_location);
      for (const auto &qLabel : register_location) {
        m_qubitLabels.emplace(qLabel);
      }

      std::cout << "Process: " << gateKey << "\n";
      auto noise_channels = noise_info["noise_channels"];
      std::vector<KrausChannel> noise_ops;
      for (auto channel_iter = noise_channels.begin();
           channel_iter != noise_channels.end(); ++channel_iter) {
        KrausChannel newOp;
        auto channel = *channel_iter;
        if (channel.find("noise_qubits") != channel.end()) {
          newOp.bit_locs =
              channel["noise_qubits"].get<std::vector<std::string>>();
        } else {
          newOp.bit_locs = register_location;
        }

        const auto op_mats =
            channel["matrix"]
                .get<std::vector<std::vector<std::vector<std::pair<double, double>>>>>();
        for (const auto &op_mat : op_mats) {
          const auto nbRows = op_mat.size();
          if (nbRows != (1ULL << newOp.bit_locs.size())) {
            xacc::error("Kraus operator matrix dimension doesn't match the "
                        "number of qubits.");
          }

          Eigen::MatrixXcd mat = Eigen::MatrixXcd::Zero(nbRows, nbRows);
          for (int row = 0; row < nbRows; ++row) {
            const auto &rowVec = op_mat[row];
            if (rowVec.size() != nbRows) {
              xacc::error("Kraus operator matrix must be square.");
            }

            for (int col = 0; col < rowVec.size(); ++col) {
              auto elemPair = rowVec[col];
              mat(row, col) =
                  std::complex<double>{elemPair.first, elemPair.second};
            }
          }

          newOp.matrix.emplace_back(mat);
        }

        if (!validateKrausCPTP(newOp.matrix)) {
          xacc::error("The list of Kraus operators for gate " + gateKey +
                      " don't satisfy the CPTP condition.");
        }
        noise_ops.emplace_back(newOp);
      }

      m_gateNoise.emplace(gateKey, noise_ops);
    }
  }

  virtual std::string toJson() const override {
    // Convert to IBM noise model Json
    nlohmann::json noiseModel;
    std::vector<nlohmann::json> noiseElements;
    // TODO: adds RO errors
    // Add Kraus noise:
    // Note: we must add noise ops for u2, u3, and cx gates:
    // (1) Single-qubit gate noise:
    for (const auto &qbitLabel : m_qubitLabels) {
      // To convert to IBM, the qubit label must be integers.
      const size_t qIdx = std::stoi(qbitLabel);
      // For mapping purposes:
      // U2 == Hadamard gate
      // U3 == X gate
      xacc::quantum::Hadamard gateU2({qIdx});
      xacc::quantum::X gateU3({qIdx});
      const std::unordered_map<std::string, xacc::quantum::Gate *> gateMap{
          {"u2", &gateU2}, {"u3", &gateU3}};

      for (const auto &[gateName, gate] : gateMap) {        
        const auto errorChannels = gateError(*gate);
        if (!errorChannels.empty()) {
          nlohmann::json element;
          element["type"] = "qerror";
          element["operations"] = std::vector<std::string>{gateName};
          element["gate_qubits"] =
              std::vector<std::vector<std::size_t>>{{qIdx}};
          std::vector<nlohmann::json> krausOps;
          std::vector<cMatPair> kraus_list;
          for (const auto &error : errorChannels) {
            kraus_list.emplace_back(convertToMatOfPairs(error.mats));
          }
          nlohmann::json instruction;
          instruction["name"] = "kraus";
          instruction["qubits"] = std::vector<std::size_t>{0};
          instruction["params"] = kraus_list;
          krausOps.emplace_back(instruction);
          element["instructions"] =
              std::vector<std::vector<nlohmann::json>>{krausOps};
          element["probabilities"] = std::vector<double>{1.0};
          noiseElements.push_back(element);
        }
      }
    }

    noiseModel["errors"] = noiseElements;
    return noiseModel.dump(6);
  }

  virtual RoErrors readoutError(size_t qubitIdx) const override { return {}; }
  virtual std::vector<RoErrors> readoutErrors() const override { return {}; }
  virtual std::vector<KrausOp>
  gateError(xacc::quantum::Gate &gate) const override {
    std::string gateKey = gate.name() + "_" + std::to_string(gate.bits()[0]);
    if (gate.bits().size() > 1) {
      for (int i = 1; i < gate.bits().size(); ++i) {
        gateKey += ("_" + std::to_string(gate.bits()[i]));
      }
    }
    const auto iter = m_gateNoise.find(gateKey);
    // Cannot find the noise info:
    if (iter == m_gateNoise.end()) {
      std::cout << "Failed to find noise configuration for gate " << gate.toString() << "\n";
      // Just assume no noise.
      return {};
    }

    const auto noiseChans = iter->second;
    std::vector<KrausOp> result;
    for (const auto &chan : noiseChans) {
      for (const auto &op : chan.matrix) {
        KrausOp newOp;
        // TODO: extend to multi-qubit ops
        newOp.qubit = std::stoi(chan.bit_locs[0]);
        newOp.mats = convertToStdMat(op);
        result.emplace_back(std::move(newOp));
      }
    }
    return result;
  }

  virtual double gateErrorProb(xacc::quantum::Gate &gate) const override {
    return 0.0;
  }
  // Query Fidelity information:
  virtual size_t nQubits() const override { return 0; }
  virtual std::vector<double> averageSingleQubitGateFidelity() const override {
    return {};
  }
  virtual std::vector<std::tuple<size_t, size_t, double>>
  averageTwoQubitGateFidelity() const override {
    return {};
  }

private:
  std::string
  createGateLookupKey(const std::string &in_gateName,
                      const std::vector<std::string> &in_qubitLocs) {
    std::string result = in_gateName + "_" + in_qubitLocs[0];
    for (int i = 1; i < in_qubitLocs.size(); ++i) {
      result = result + "_" + in_qubitLocs[i];
    }
    return result;
  }

private:
  nlohmann::json m_noiseModel;
  BitOrder m_bitOrder;
  std::unordered_map<std::string, std::vector<KrausChannel>> m_gateNoise;
  std::set<std::string> m_qubitLabels;
};
} // namespace xacc

REGISTER_PLUGIN(xacc::JsonNoiseModel, xacc::NoiseModel)