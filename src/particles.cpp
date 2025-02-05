/*******************************************************************************
    Copyright (c) The Taichi MPM Authors (2018- ). All Rights Reserved.
    The use of this software is governed by the LICENSE file.
*******************************************************************************/

#include <pybind11/pybind11.h>
#include <taichi/dynamics/rigid_body.h>
#include "particles.h"

TC_NAMESPACE_BEGIN

inline MatrixND<2, real> dR_from_dF(const MatrixND<2, real> &F,
                                    const MatrixND<2, real> &R,
                                    const MatrixND<2, real> &S,
                                    const MatrixND<2, real> &dF) {
  using Matrix = MatrixND<2, real>;
  using Vector = VectorND<2, real>;

  // set W = R^T dR = [  0    x  ]
  //                  [  -x   0  ]
  //
  // R^T dF - dF^T R = WS + SW
  //
  // WS + SW = [ x(s21 - s12)   x(s11 + s22) ]
  //           [ -x[s11 + s22]  x(s21 - s12) ]
  // ----------------------------------------------------
  Matrix lhs = transposed(R) * dF - transposed(dF) * R;
  real x;
  real abs0 = abs(S[0][0] + S[1][1]);
  real abs1 = abs(S[0][1] - S[1][0]);
  if (abs0 > abs1) {
    x = lhs[1][0] / (S[0][0] + S[1][1]);
  } else {
    x = lhs[0][0] / (S[0][1] - S[1][0]);
  }
  Matrix W = Matrix(Vector(0, -x), Vector(x, 0));
  return R * W;
};

// viscous ---------------------------------------------------------------------
template <int dim>
class ViscoParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  real visco_tau;
  real visco_nu;
  real visco_kappa;
  real lambda_0, mu_0;
  real dt;
  TC_IO_DEF_WITH_BASE(visco_tau, visco_nu, visco_kappa, lambda_0, mu_0, dt);

  ViscoParticle() : Base() {
  }

  void initialize(const Config &config) override {
    Base::initialize(config);

    real E = config.get("youngs_modulus", 4e4_f);
    real nu = config.get("poisson_ratio", 0.4_f);
    lambda_0 = E * nu / ((1.0_f + nu) * (1.0_f - 2.0_f * nu));
    mu_0 = E / (2.0_f * (1.0_f + nu));

    visco_tau = config.get("tau", 1000.0_f);
    visco_nu = config.get("nu", 10000.0_f);
    visco_kappa = config.get("kappa", 0.0_f);

    dt = config.get("base_delta_t", 1e-4f);
  }

  virtual Matrix first_piola_kirchhoff() override {
    real j_e = determinant(this->dg_e);
    real mu = mu_0, lambda = lambda_0;
    Matrix r, s;
    polar_decomp(this->dg_e, r, s);
    Matrix grad = 2 * mu * (this->dg_e - r) +
                  lambda * (j_e - 1) * j_e * inverse(transpose(this->dg_e));
    return grad;
  }

  Matrix calculate_force() override {
    Matrix grad = first_piola_kirchhoff();
    return -this->vol * grad * transpose(this->dg_e);
  }

  Matrix approximate_exponent(real dt, const Matrix &m) {
    Matrix s = m * dt;
    Matrix r = (s * 0.5_f + Matrix(1.0_f)) * s + Matrix(1.0_f);

    if (determinant(r) > 0.0_f)
      return r;
    Matrix tmp = approximate_exponent(dt / 2, m);

    //    for (int i = 0; i < dim; ++i)
    //      for (int j = 0; j < dim; ++j)
    //        tmp[i][j] = tmp[i][j] * tmp[i][j];

    return tmp * tmp;
  }

  virtual int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    Matrix dg_e_hat =
        approximate_exponent(dt, (cdg - Matrix(1.0_f)) * (1.0_f / dt)) *
        this->dg_e;
    Matrix svd_u, sig, svd_v;
    svd(dg_e_hat, svd_u, sig, svd_v);
    real pnorm = first_piola_kirchhoff().frobenius_norm();
    real gamma = 0.0_f;

    if (pnorm > 1e-5_f)
      gamma = clamp(dt * visco_nu * (pnorm - visco_tau) / pnorm, 0.0_f, 1.0_f);
    real scale = 1.0_f;
    if (abs(determinant(sig)) > 1e-5_f)
      scale = 1.0_f / pow(determinant(sig), 1.0_f / real(dim));
    Matrix middle(0.0_f), middle_inv(0.0_f);
    for (int d = 0; d < dim; ++d)
      middle[d][d] = pow(sig[d][d] * scale, gamma);
    for (int d = 0; d < dim; ++d)
      if (abs(middle[d][d]) > 1e-5_f)
        middle_inv[d][d] = 1.0_f / middle[d][d];
      else
        middle_inv[d][d] = 1.0_f;
    this->dg_e = svd_u * sig * middle_inv * transpose(svd_v);

    svd(this->dg_e, svd_u, sig, svd_v);
    for (int d = 0; d < dim; ++d)
      sig[d][d] = clamp(sig[d][d], 0.1_f, 10.0_f);
    this->dg_e = svd_u * sig * transpose(svd_v);

    visco_tau += visco_kappa * gamma * pnorm;

    return 0;
  };

  virtual real get_allowed_dt(const real &dx) const override {
    real J = determinant(this->dg_e);
    real mass = this->get_mass();
    real vol0 = this->vol;
    real rho0 = mass / vol0;
    real rho = rho0 / J;

    real K = 2.0_f * mu_0 / 3.0_f + lambda_0;
    real c2 = 4.0_f * mu_0 / (3.0_f * rho) + K * (1.0_f - std::log(J)) / rho0;
    c2 = max(c2, 1e-20_f);
    real c = sqrt(c2);
    //
    //    real c = sqrt((lambda_0 + 2.0_f * mu_0)/rho);

    Vector v = this->get_velocity();
    real u = sqrt(v.dot(v));

    return dx / (c + u);
  }

  Vector3 get_debug_info() const override {
    return Vector3(0, 1, 0);
  }

  std::string get_name() const override {
    return "visco";
  }
};

// snow ------------------------------------------------------------------------
template <int dim>
class SnowParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  real Jp;  // determinant of dg_p = determinant(plastic deformation gradient)
  real hardening;
  real mu_0;
  real lambda_0;
  real theta_c;
  real theta_s;
  real min_Jp;
  real max_Jp;
  TC_IO_DEF_WITH_BASE(Jp,
                      hardening,
                      mu_0,
                      lambda_0,
                      theta_c,
                      theta_s,
                      min_Jp,
                      max_Jp);

  SnowParticle() : Base() {
  }

  void initialize(const Config &config) override {
    Base::initialize(config);
    hardening = config.get("hardening", 10.0_f);
    real E = config.get("youngs_modulus", 1.4e5_f);
    real nu = config.get("poisson_ratio", 0.2_f);
    lambda_0 =
        config.get("lambda_0", E * nu / ((1.0_f + nu) * (1.0_f - 2.0_f * nu)));
    mu_0 = config.get("mu_0", E / (2.0_f * (1.0_f + nu)));
    theta_c = config.get("theta_c", 2.5e-2_f);
    theta_s = config.get("theta_s", 7.5e-3_f);
    min_Jp = config.get("min_Jp", 0.6_f);
    max_Jp = config.get("max_Jp", 20.0_f);
    Jp = config.get("Jp", 1.0_f);
  }

  virtual Matrix first_piola_kirchhoff() override {
    real j_e = determinant(this->dg_e);
    auto lame = get_lame_parameters();
    real mu = lame.first, lambda = lame.second;
    Matrix r, s;
    polar_decomp(this->dg_e, r, s);
    Matrix grad = 2 * mu * (this->dg_e - r) +
                  lambda * (j_e - 1) * j_e * inverse(transpose(this->dg_e));
    return grad;
  }

  Matrix calculate_force() override {
    return -this->vol * first_piola_kirchhoff() * transpose(this->dg_e);
  }

  virtual int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    this->dg_e = cdg * this->dg_e;
    real dg_e_det_orig = 1.0_f;
    real dg_e_det = 1.0_f;
    Matrix svd_u, sig, svd_v;
    svd(this->dg_e, svd_u, sig, svd_v);
    for (int i = 0; i < dim; i++) {
      dg_e_det_orig *= sig[i][i];
      sig[i][i] = clamp(sig[i][i], 1.0_f - theta_c, 1.0_f + theta_s);
      dg_e_det *= sig[i][i];
    }
    this->dg_e = svd_u * sig * transposed(svd_v);

    real Jp_new = Jp * dg_e_det_orig / dg_e_det;
    if (!(Jp_new <= max_Jp))
      Jp_new = max_Jp;
    if (!(Jp_new >= min_Jp))
      Jp_new = min_Jp;
    Jp = Jp_new;
    return 0;
  }

  std::pair<real, real> get_lame_parameters() const {
    // real e = std::max(1e-7f, std::exp(std::min(hardening * (1.0f - j_p),
    // 5.0f)));
    // no clamping
    real e = std::exp(hardening * (1.0_f - Jp));
    real mu = mu_0 * e;
    real lambda = lambda_0 * e;
    return {mu, lambda};
  }

  virtual real get_allowed_dt(const real &dx) const override {
    real J = determinant(this->dg_e) * Jp;
    real mass = this->get_mass();
    real vol0 = this->vol;
    real rho0 = mass / vol0;
    real rho = rho0 / J;

    auto lame = get_lame_parameters();
    real mu = lame.first, lambda = lame.second;
    real c = sqrt((lambda + 2.0_f * mu) / rho);

    if (c != c) {
      TC_WARN("{} {}", determinant(this->dg_e), Jp);
      TC_WARN("{}", rho);
      TC_WARN("lambda : {}, mu : {}", lambda, mu);
      TC_STOP;
    }

    //    real c = sqrt((lambda_0 + 2.0_f * mu_0)/rho);

    Vector v = this->get_velocity();
    real u = sqrt(v.dot(v));

    return dx / (c + u);
  }

  virtual Matrix get_first_piola_kirchoff_differential(
      const Matrix &dF) override;

  virtual real get_stiffness() const override {
    auto lame = get_lame_parameters();
    return this->vol * (lame.first + 2 * lame.second);
  }

  Vector3 get_debug_info() const override {
    return Vector3(0, 2, 0);
  }

  std::string get_name() const override {
    return "snow";
  }
};

// linear ---------------------------------------------------------------------/
template <int dim>
class LinearParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  using Base::dg_e;

  real E;
  real nu;
  real mu;
  real lambda;
  TC_IO_DEF_WITH_BASE(E, nu, mu, lambda);

  LinearParticle() : Base() {
  }

  void initialize(const Config &config) override {
    Base::initialize(config);
    E = config.get("E", 1e5_f);
    nu = config.get("nu", 0.3_f);
    mu = E / (2 * (1 + nu));
    lambda = E * nu / ((1 + nu) * (1 - 2 * nu));
  }

  virtual real potential_energy() const override {
    auto e = 0.5_f * (dg_e + transposed(dg_e)) - Matrix(1);
    return this->vol *
           (mu * e.frobenius_norm2() + 0.5_f * lambda * sqr(e.trace()));
  }

  virtual Matrix first_piola_kirchhoff() override {
    return mu * (dg_e + transposed(dg_e) - Matrix(2.0_f)) +
           Matrix(lambda * (dg_e.trace() - dim));
  }

  Matrix calculate_force() override {
    return -this->vol * first_piola_kirchhoff() * transpose(dg_e);
  }

  int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    dg_e = cdg * dg_e;
    return 0;
  }

  real get_allowed_dt(const real &dx) const override {
    return 0.0f;
  }

  Vector3 get_debug_info() const override {
    return Vector3(0, 3, 0);
  }

  std::string get_name() const override {
    return "linear";
  }

  virtual void set_mu_to_zero() {
    mu = 0;
  }

  virtual void set_lambda_and_mu_to_zero() {
    mu = 0;
    lambda = 0;
  }
};

// jelly -----------------------------------------------------------------------
template <int dim>
class JellyParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  using Base::dg_e;

  real E;
  real nu;
  real mu;
  real lambda;
  TC_IO_DEF_WITH_BASE(E, nu, mu, lambda);

  JellyParticle() : Base() {
  }

  void initialize(const Config &config) override {
    Base::initialize(config);
    E = config.get("E", 1e5_f);
    nu = config.get("nu", 0.3_f);
    mu = E / (2 * (1 + nu));
    lambda = E * nu / ((1 + nu) * (1 - 2 * nu));
  }

  Matrix first_piola_kirchhoff() override {
    real j = determinant(dg_e);
    Matrix r, s;
    polar_decomp(dg_e, r, s);
    Matrix grad =
        2 * mu * (dg_e - r) + lambda * (j - 1) * j * inverse(transpose(dg_e));
    return grad;
  }

  real potential_energy() const override {
    real J = determinant(dg_e);
    Matrix r, s;
    polar_decomp(dg_e, r, s);
    return (mu * (dg_e - r).frobenius_norm2() +
            0.5_f * lambda * pow<2>(J - 1.0_f)) *
           this->vol;
  }

  Matrix calculate_force() override {
    return -this->vol * first_piola_kirchhoff() * transpose(dg_e);
  }

  int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    dg_e = cdg * dg_e;
    return 0;
  }

  real get_allowed_dt(const real &dx) const override {
    return 0.0f;
  }

  Vector3 get_debug_info() const override {
    return Vector3(0, 4, 0);
  }

  std::string get_name() const override {
    return "jelly";
  }

  virtual void set_mu_to_zero() {
    mu = 0;
  }

  virtual void set_lambda_and_mu_to_zero() {
    mu = 0;
    lambda = 0;
  }
};

// water -----------------------------------------------------------------------
template <int dim>
class WaterParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;
  using Base::debug;

  real k = 10000.f;
  real gamma = 7.f;
  real j;
  TC_IO_DEF_WITH_BASE(k, gamma, j);

  WaterParticle() : Base() {
  }

  void initialize(const Config &config) override {
    Base::initialize(config);
    k = config.get("k", k);
    gamma = config.get("gamma", gamma);
    j = 1.f;
  }

  Matrix calculate_force() override {
    real p = k * (std::pow(j, -gamma) - 1.f);
    Matrix sigma = -p * Matrix(1.f);
    return -this->vol * j * sigma;
  }

  int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    j *= cdg.diag().sum() - (dim - 1);
    const real threshold = 0.1_f;
    if (j < threshold) {
      TC_INFO("liquid particle j = {}, position = ({}, {}, {})", j,
              this->pos[0], this->pos[1], this->pos[2]);
      j = threshold;
    }
    return bool(int(j > 1));
  }

  real get_allowed_dt(const real &dx) const override {
    real c2 = k * gamma / std::pow(j, gamma - 1);
    real c = sqrt(c2);

    //    real c = sqrt((lambda_0 + 2.0_f * mu_0)/rho);

    Vector v = this->get_velocity();
    real u = sqrt(v.dot(v));

    return dx / (c + u);
  }

  std::string get_name() const override {
    return "water";
  }

  Vector3 get_debug_info() const override {
    return Vector3(j, 5, (int)this->sticky);
  }
};

template <>
MatrixND<3, real> SnowParticle<3>::get_first_piola_kirchoff_differential(
    const MatrixND<3, real> &dF) {
  TC_NOT_IMPLEMENTED;
  return Matrix(0.0f);
};

template <>
MatrixND<2, real> SnowParticle<2>::get_first_piola_kirchoff_differential(
    const MatrixND<2, real> &dF) {
  const real j_e = determinant(this->dg_e);
  const real j_p = Jp;
  const real e = expf(hardening * (1.0f - j_p));
  const real mu = mu_0 * e;
  const real lambda = lambda_0 * e;
  const auto F = this->dg_e;
  Matrix r, s;
  polar_decomp(this->dg_e, r, s);
  Matrix dR = dR_from_dF(this->dg_e, r, s, dF);
  Matrix JFmT = Matrix(Vector(F[1][1], -F[1][0]), Vector(-F[0][1], F[0][0]));
  Matrix dJFmT =
      Matrix(Vector(dF[1][1], -dF[1][0]), Vector(-dF[0][1], dF[0][0]));
  return 2.0_f * mu * (dF - dR) +
         lambda * JFmT * (JFmT.elementwise_product(dF)).sum() +
         lambda * (j_e - 1) * dJFmT;
};

inline real clamp_small_magnitude(const real x, const real eps) {
  assert(eps >= 0);
  if (x < -eps)
    return x;
  else if (x < 0)
    return -eps;
  else if (x < eps)
    return eps;
  else
    return x;
}

inline real log_1px_over_x(const real x, const real eps) {
  assert(eps > 0);
  if (std::fabs(x) < eps)
    return (real)1;
  else
    return std::log1p(x) / x;
}

inline real diff_log_over_diff(const real x, const real y, const real eps) {
  assert(eps > 0);
  real p = x / y - 1;
  return log_1px_over_x(p, eps) / y;
}

inline real diff_interlock_log_over_diff(const real x,
                                         const real y,
                                         const real logy,
                                         const real eps) {
  assert(eps > 0);
  return logy - y * diff_log_over_diff(x, y, eps);
}

// sand ------------------------------------------------------------------------
// StvkWithHencky with volume correction
template <int dim>
class SandParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  real lambda_0 = 204057.0_f, mu_0 = 136038.0_f;
  real friction_angle = 30.0_f;
  real alpha = 1.0_f;
  real cohesion = 0.0_f;
  real logJp = 0.0_f;
  real beta = 1.0_f;
  TC_IO_DEF_WITH_BASE(lambda_0,
                      mu_0,
                      friction_angle,
                      alpha,
                      cohesion,
                      logJp,
                      beta);

  SandParticle() : MPMParticle<dim>() {
  }

  void initialize(const Config &config) override {
    Base::initialize(config);
    lambda_0 = config.get("lambda_0", lambda_0);
    mu_0 = config.get("mu_0", mu_0);
    friction_angle = config.get("friction_angle", 30._f);
    real sin_phi = std::sin(friction_angle / 180._f * real(3.141592653));
    alpha = std::sqrt(2._f / 3._f) * 2._f * sin_phi / (3._f - sin_phi);
    cohesion = config.get("cohesion", 0._f);
    logJp = 0._f;
    beta = config.get("beta", 1.0_f);
  }

  // project : called from plasticity func -------------------------------------
  void project(Matrix sigma, real alpha, Matrix &sigma_out) {
    const real d = dim;
    Vector epsilon_diag;
    for (int i = 0; i < dim; i++) {
      epsilon_diag[i] =
          std::log(std::max(std::abs(sigma[i][i]), 1e-4_f)) - cohesion;
    }
    Matrix epsilon(epsilon_diag);
    real tr = epsilon.diag().sum() + logJp;
    Matrix epsilon_hat = epsilon - (tr) / d * Matrix(1.0_f);
    real epsilon_hat_for = epsilon_hat.diag().length();
    // case II
    if (tr >= 0.0_f) {
      sigma_out = Matrix(std::exp(cohesion));
      logJp = beta * epsilon.diag().sum() + logJp;
      // this->sCase = 20; // stress case
    } else {
      logJp = 0;
      real delta_gamma =
          epsilon_hat_for + (d * lambda_0 + 2 * mu_0) / (2 * mu_0) * tr * alpha;
      // case I
      if (delta_gamma <= 0) {
        Matrix h = epsilon + Matrix(cohesion);
        sigma_out = Matrix(h.diag().map(static_cast<real (*)(real)>(std::exp)));
        // this->sCase = 10; // stress case
      // case III
      } else {
        Matrix h = epsilon - delta_gamma / epsilon_hat_for * epsilon_hat +
                   Matrix(cohesion);
        sigma_out = Matrix(h.diag().map(static_cast<real (*)(real)>(std::exp)));
        // this->sCase = 30; // stress case
      }
    }
  }

  Matrix calculate_force() override {
    Matrix u, v, sig, dg = this->dg_e;
    svd(this->dg_e, u, sig, v);
    Matrix log_sig(
        sig.diag().template map(static_cast<real (*)(real)>(std::log)));
    Matrix inv_sig(Vector(1.0_f) / sig.diag());
    Matrix center = 2.0_f * mu_0 * inv_sig * log_sig +
                    lambda_0 * (log_sig.diag().sum()) * inv_sig;

    // added -------------------------------------------------------------------
    // Matrix cS = -(1/determinant(this->dg_e)) *        // Cauchy Stress tensor
    //             (u * center * transpose(v)) * transpose(dg);
    // this->sm  = (cS[1][1] + cS[2][2] + cS[3][3]) / 3.0f;
    // this->j2s = sqrt( (pow(cS[1][1],(real)2)+
    //                    pow(cS[2][2],(real)2)+
    //                    pow(cS[3][3],(real)2))/3.0_f -
    //                   (cS[1][1]*cS[2][2]+
    //                    cS[1][1]*cS[3][3]+
    //                    cS[3][3]*cS[2][2])/3.0_f +
    //                   (pow(cS[1][2],(real)2)+
    //                    pow(cS[2][3],(real)2)+
    //                    pow(cS[3][1],(real)2)) );

    return -this->vol * (u * center * transpose(v)) * transpose(dg);
  }

  int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    this->dg_e = cdg * this->dg_e;
    Matrix u, v, sig;
    svd(this->dg_e, u, sig, v);
    Matrix t(1.0_f);
    project(sig, alpha, t);
    this->dg_e = u * t * transposed(v);
    return 0;
  }

  real get_allowed_dt(const real &dx) const override {
    real J = determinant(this->dg_e); // J -------------------------------------
    real mass = this->get_mass();
    real vol0 = this->vol;
    real rho0 = mass / vol0;
    real rho = rho0 / J;

    real K = 2.0_f * mu_0 / 3.0_f + lambda_0;
    real c2 = 4.0_f * mu_0 / (3.0_f * rho) + K * (1.0_f - std::log(J)) / rho0;
    c2 = max(c2, 1e-20_f);
    real c = sqrt(c2);

    //    real c = sqrt((lambda_0 + 2.0_f * mu_0)/rho);

    Vector v = this->get_velocity();
    real u = sqrt(v.dot(v));

    return dx / (c + u);
  }

  Vector3 get_debug_info() const override {
    return Vector3(0, 6, 0);
  }

  std::string get_name() const override {
    return "sand";
  }
};

// VonMises --------------------------------------------------------------------
// StvkWithHencky with volume correction
template <int dim>
class VonMisesParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  real lambda_0;
  real mu_0;
  real yield_stress;
  TC_IO_DEF_WITH_BASE(lambda_0, mu_0, yield_stress);

  void initialize(const Config &config) override {
    Base::initialize(config);
    real youngs_modulus = config.get("youngs_modulus", 5e3_f);
    real poisson_ratio = config.get("poisson_ratio", 0.4_f);
    lambda_0 = youngs_modulus * poisson_ratio /
               ((1.0_f + poisson_ratio) * (1.0_f - 2.0_f * poisson_ratio));
    mu_0 = youngs_modulus / (2.0_f * (1.0_f + poisson_ratio));
    yield_stress = config.get("yield_stress", 1.0_f);
  }

  Matrix calculate_force() override {
    Matrix u, v, sig, dg = this->dg_e;
    svd(this->dg_e, u, sig, v);
    Matrix log_sig(
        sig.diag().template map(static_cast<real (*)(real)>(std::log)));
    Matrix inv_sig(Vector(1.0_f) / sig.diag());
    Matrix center = 2.0_f * mu_0 * inv_sig * log_sig +
                    lambda_0 * (log_sig.diag().sum()) * inv_sig;

    return -this->vol * (u * center * transpose(v)) * transpose(dg);
  }

  int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    this->dg_e = cdg * this->dg_e;
    Matrix U, V, sigma;
    svd(this->dg_e, U, sigma, V);
    Matrix epsilon(
        sigma.diag().template map(static_cast<real (*)(real)>(std::log)));
    real trace_epsilon = epsilon.trace();
    Matrix epsilon_hat = epsilon - (trace_epsilon / (real)dim) * Matrix(1.0_f);
    real epsilon_hat_norm = epsilon_hat.frobenius_norm2();
    real delta_gamma = epsilon_hat_norm - yield_stress / (2.0_f * mu_0);
    if (delta_gamma <= 0)  // case I
    {
      return 0;
    }
    Matrix H =
        epsilon - (delta_gamma / epsilon_hat_norm) * epsilon_hat;  // case II
    Matrix exp_H = Matrix(H.diag().map(static_cast<real (*)(real)>(std::exp)));
    this->dg_e = U * exp_H * transposed(V);
    return 1;
  }

  real get_allowed_dt(const real &dx) const override {
    real J = determinant(this->dg_e);
    real mass = this->get_mass();
    real vol0 = this->vol;
    real rho0 = mass / vol0;
    real rho = rho0 / J;

    real K = 2.0_f * mu_0 / 3.0_f + lambda_0;
    real c2 = 4.0_f * mu_0 / (3.0_f * rho) + K * (1.0_f - std::log(J)) / rho0;
    c2 = max(c2, 1e-20_f);
    real c = sqrt(c2);

    //    real c = sqrt((lambda_0 + 2.0_f * mu_0)/rho);

    Vector v = this->get_velocity();
    real u = sqrt(v.dot(v));

    return dx / (c + u);
  }

  Vector3 get_debug_info() const override {
    return Vector3(0, 7, 0);
  }

  std::string get_name() const override {
    return "von_mises";
  }
};

// elastic ---------------------------------------------------------------------
// StvkWithHencky with volume correction
template <int dim>
class ElasticParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  real lambda_0;
  real mu_0;
  real E;
  real nu;
  TC_IO_DEF_WITH_BASE(lambda_0, mu_0, E, nu);

  void initialize(const Config &config) override {
    Base::initialize(config);
    E = config.get("E", 5e3_f);
    nu = config.get("nu", 0.4_f);
    lambda_0 = E * nu / ((1.0_f + nu) * (1.0_f - 2.0_f * nu));
    mu_0 = E / (2.0_f * (1.0_f + nu));
  }

  virtual real potential_energy() const override {
    Matrix u, v, sig;
    svd(this->dg_e, u, sig, v);
    Vector log_sigma =
        sig.diag().abs().template map(static_cast<real (*)(real)>(std::log));
    Vector log_sigma_squared;
    for (int d = 0; d < dim; ++d)
      log_sigma_squared[d] = log_sigma[d] * log_sigma[d];
    return (mu_0 * log_sigma_squared.sum() +
            0.5_f * lambda_0 * log_sigma.sum() * log_sigma.sum()) *
           this->vol;
  }

  Matrix calculate_force() override {
    Matrix u, v, sig, dg = this->dg_e;
    svd(this->dg_e, u, sig, v);
    Matrix log_sig(
        sig.diag().template map(static_cast<real (*)(real)>(std::log)));
    Matrix inv_sig(Vector(1.0_f) / sig.diag());
    Matrix center = 2.0_f * mu_0 * inv_sig * log_sig +
                    lambda_0 * (log_sig.diag().sum()) * inv_sig;

    return -this->vol * (u * center * transpose(v)) * transpose(dg);
  }

  int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    this->dg_e = cdg * this->dg_e;
    return 0;
  }

  real get_allowed_dt(const real &dx) const override {
    real J = determinant(this->dg_e);
    real mass = this->get_mass();
    real vol0 = this->vol;
    real rho0 = mass / vol0;
    real rho = rho0 / J;

    real K = 2.0_f * mu_0 / 3.0_f + lambda_0;
    real c2 = 4.0_f * mu_0 / (3.0_f * rho) + K * (1.0_f - std::log(J)) / rho0;
    c2 = max(c2, 1e-20_f);
    real c = sqrt(c2);

    //    real c = sqrt((lambda_0 + 2.0_f * mu_0)/rho);

    Vector v = this->get_velocity();
    real u = sqrt(v.dot(v));

    return dx / (c + u);
  }

  std::string get_name() const override {
    return "elastic";
  }

  Vector3 get_debug_info() const override {
    return Vector3(E, 8, 0);
  }
};


// added: Nonlocal -------------------------------------------------------------
// from Ken Kamrin at MIT, by Amin Haeri at Concordia
template <int dim>
class NonlocalParticle : public MPMParticle<dim> {
 public:
  using Base = MPMParticle<dim>;
  using Vector = typename Base::Vector;
  using Matrix = typename Base::Matrix;

  real S_mod;  // Shear modulus
  real B_mod;  // Bulk modulus
  real A_mat;
  real dia;
  real rho_s;
  real rho_c;
  real mu_s;
  real mu_2;
  real I_0;
  real t_0;
  real delta_t;

  TC_IO_DEF_WITH_BASE(S_mod,
                      B_mod,
                      A_mat,
                      dia,
                      rho_s,
                      rho_c,
                      mu_s,
                      mu_2,
                      I_0,
                      t_0,
                      delta_t);

  NonlocalParticle() : MPMParticle<dim>() {
  }

  void initialize(const Config &config) override {
    Base::initialize(config);
    // Conversion ref: https://en.wikipedia.org/wiki/Elastic_modulus
    S_mod = config.get("S_mod", 3.4483e3_f);
    B_mod = config.get("B_mod", 3.3333e4_f);
    A_mat = config.get("A_mat", 0.48_f);
    dia = config.get("dia", 0.005_f);
    rho_s = config.get("density", 2550.0_f);
    rho_c = config.get("critical_density", 2000.0_f);
    // mu_s should be larger than sqrt(3)*(1-(2*nu))/(1+nu)
    mu_s = config.get("mu_s", 0.3819_f);
    mu_2 = config.get("mu_2", 0.6435_f);
    I_0 = config.get("I_0", 0.278_f);
    t_0 = config.get("t_0", 1e-3_f);
    delta_t = config.get("base_delta_t", 1e-4_f);
  }

  // Apply force @ n
  Matrix calculate_force() override {
    // force = vol * T
    return  -this->vol * this->T;
  }

  // Calculate stress @ n+1 and update granular fluidity and deformation gradient
  int plasticity(const Matrix &cdg, const real &laplacian_gf) override {
    Matrix I = Matrix(1.0_f);
    real eps = 1e-20_f;
    real mu;
    real p_n = this->p;  // p @ n

    // Kinematics (Lambda func)
    auto kinematics = [I](auto cdg, auto delta_t) {
      Matrix L = (1.0_f / delta_t) *  // Velocity gradient (C)
                 (cdg - I);
      Matrix D = (0.5_f) * (L + transpose(L));  // Symmetric part of L
      Matrix D_0 = D; // makes flow unstable: D - D.trace()/3.0_f * I;  // Deviatoric part of D
      real gamma_dot_equ = 0.0_f;
      for (int i = 0; i < dim; ++i)
        for (int j = 0; j < dim; ++j)
          gamma_dot_equ += pow(D_0[i][j], 2.0_f);
      gamma_dot_equ = 1.414_f * sqrt(gamma_dot_equ);
      return gamma_dot_equ;  // Total equ shear strain rate @ n+1
    };

    this->dg_t = cdg * this->dg_t;  // dg_t @ n+1
    real rho = this->get_mass() / this->vol / determinant(this->dg_t);

    // this->dg_p_det = determinant(this->dg_p);
    // this->dg_p_inv_det = determinant(inverse(this->dg_p));

    Matrix dg_el = this->dg_t * inverse(this->dg_p);  // dg_e @ tr
    Matrix u, v, sig;
    svd(dg_el, u, sig, v);
    Matrix Re = u * transpose(v);
    // Ue <- v * sig * transpose(v)

    Matrix log_sig(
      sig.diag().template map(static_cast<real (*)(real)>(std::log)));

    Matrix Ee = v * log_sig * transpose(v);  // Ee @ tr
    real trEe = Ee.trace();
    Matrix Ee_0 = Ee - (trEe / 3.0_f * I);

    Matrix Me = (2.0_f * S_mod * Ee_0) + (B_mod * trEe * I);  // Me @ tr
    // Matrix Me_trial = Me;

    this->p = - Me.trace() / 3.0_f;  // p @ n+1
    Matrix Np;

    // Disconnected
    if (rho < rho_c || this->p <= 0.0_f){
      // this->is_free = true;
      this->T = Matrix(0.0_f);
      this->dg_p = this->dg_t;
      this->p = 0.0_f;  // For tagging
      mu = mu_2;  // For visualization
      this->gf = std::max(0.0_f, kinematics(cdg, delta_t) / mu_2);  // For visualization
    }
    // Dense
    else
    {
      // TC_P(determinant(this->dg_t));
      // this->is_free = false;

      mu = std::min(this->tau / p_n, mu_2-eps);  // mu @ n
      real gdot_loc = - ((mu_s - mu) * this->gf)
                      - ((mu_2 - mu_s) / I_0 *
                          sqrt(rho_s * pow(dia, 2.0_f) / p_n) *
                          mu * pow(this->gf, 2.0_f));
      real gdot_nonloc = pow(A_mat, 2.0_f) * pow(dia, 2.0_f) * laplacian_gf;
      this->gf = std::max(0.0_f, (delta_t*(gdot_loc+gdot_nonloc)/t_0) + this->gf);

      Matrix Me_0 = Me + this->p * I;
      real Me_0_mag = 0.0_f;
      for (int i = 0; i < dim; ++i)
        for (int j = 0; j < dim; ++j)
          Me_0_mag += pow(Me_0[i][j], 2.0_f);
      Me_0_mag = sqrt(Me_0_mag);

      real tau_trial = 0.707_f * Me_0_mag;  // tau @ tr
      
      if (tau_trial > 0.0_f){
        Np = (0.707_f / tau_trial) * Me_0;  // Np @ tr
      }else{
        Np = Matrix(0.0_f);
      }

      // modify gf ?
      if (p_n == 0.0_f)
      {
        this->gf = std::max(0.0_f, kinematics(cdg, delta_t) / mu_2);

        // real gdot_nonloc = pow(A_mat, 2.0_f) * pow(dia, 2.0_f) * laplacian_gf;
        // this->gf = std::max(0.0_f, (delta_t * gdot_nonloc / t_0));
        // this->gf = std::max(0.0_f, (delta_t * gdot_nonloc / t_0) + this->gf);

        // this->gf = 0.0_f;
        // this->dg_p = I;

        // mu = std::max(std::min(tau_trial / this->p, mu_2-eps), eps);
        // if (mu < mu_s){
        //   this->gf = 0.0_f;
        // }
        // else{
        //   this->gf = sqrt(this->p/rho_s/pow(dia,2.0_f)) * (mu - mu_s) / ((mu_2 - mu_s) / I_0) / mu;
        //   // TC_P(this->gf);
        // }
      }

      this->tau =  // tau @ n+1
        tau_trial * this->p /  // gf @ n+1
        std::max(this->p + S_mod * delta_t * this->gf, eps);

      if (this->tau < 0.0_f)
        this->tau = 0.0_f;
      // No plastic deformation if elastic deformation
      // if (this->tau > tau_trial || tau_trial <= mu_s*this->p)   // removed ?
      if (this->tau > tau_trial)
        this->tau = tau_trial;

      Me = Me - 1.414_f * (tau_trial-this->tau) * Np;  // Me @ n+1

      if (p_n > 0.0_f){  // p @ n
        mu = std::min(this->tau/std::max(this->p, eps), mu_2-eps);  // mu @ n+1
      }
      else{
        mu = mu_2;
      }

      this->T =  // T @ n+1
        (1/ determinant(this->dg_t)) * Re * Me * transpose(Re);

      this->dg_p =  // dg_p @ n+1
        (I + delta_t * 0.707_f * mu * this->gf * Np) * this->dg_p;

      // elastic deformation
      // if (tau_trial <= mu_s*this->p){
      //   mu = 0.0_f;  // no plastic deformation gradient update if elastic deformation
      // }
    }

    // this->mu_visual = mu;
    return 0;
  }

  real get_allowed_dt(const real &dx) const override {
    real J = determinant(this->dg_t);
    real mass = this->get_mass();
    real vol0 = this->vol;
    real rho0 = mass / vol0;
    real rho = rho0 / J;

    real lambda_0 = 204057.0_f, mu_0 = 136038.0_f;  // check it

    real K = 2.0_f * mu_0 / 3.0_f + lambda_0;
    real c2 = 4.0_f * mu_0 / (3.0_f * rho) + K * (1.0_f - std::log(J)) / rho0;
    c2 = max(c2, 1e-20_f);
    real c = sqrt(c2);

    Vector v = this->get_velocity();
    real u = sqrt(v.dot(v));

    return dx / (c + u);
  }

  Vector3 get_debug_info() const override {
    return Vector3(0, 6, 0);
  }

  std::string get_name() const override {
    return "nonlocal";
  }
}; 

// Interface definition and implementation registration

TC_INTERFACE_DEF(MPMParticle2D, "mpm_particle_2d");
TC_INTERFACE_DEF(MPMParticle3D, "mpm_particle_3d");

TC_REGISTER_MPM_PARTICLE(Visco);
TC_REGISTER_MPM_PARTICLE(Snow);
TC_REGISTER_MPM_PARTICLE(Linear);
TC_REGISTER_MPM_PARTICLE(Jelly);
TC_REGISTER_MPM_PARTICLE(Water);
TC_REGISTER_MPM_PARTICLE(Sand);
TC_REGISTER_MPM_PARTICLE(VonMises);
TC_REGISTER_MPM_PARTICLE(Elastic);
TC_REGISTER_MPM_PARTICLE(Nonlocal);  // added

TC_NAMESPACE_END
