#include <vector>
#include <fstream>
#include <sstream>
#include <exception>

#include <vexcl/devlist.hpp>
#include <vexcl/vector.hpp>
#include <vexcl/multivector.hpp>
#include <vexcl/generator.hpp>
#include <vexcl/element_index.hpp>
#include <vexcl/profiler.hpp>

#include <boost/array.hpp>
#include <boost/numeric/odeint.hpp>
#include <boost/program_options.hpp>

namespace odeint = boost::numeric::odeint;

struct maxeler_constant_var {
    std::string repr;

    template <class T>
    maxeler_constant_var(const T &val) {
        std::ostringstream s;
        s << "constant.var(" << val << ")";
        repr = s.str();
    }

    friend std::ostream & operator<<(std::ostream &s, const maxeler_constant_var &v) {
        return s << v.repr;
    }
};

namespace vex {
    template <> struct is_cl_native<maxeler_constant_var> : std::true_type {};
}

typedef vex::symbolic< double > sym_var;
typedef vex::symbolic< std::array<double,14> > sym_state;

template <class LHS, class RHS>
void connect(const LHS &lhs, const RHS &rhs) {
    vex::generator::detail::symbolic_context ctx;

    vex::generator::get_recorder() << "\t\t";

    boost::proto::eval(boost::proto::as_expr(lhs), ctx);
    vex::generator::get_recorder() << " <== ";
    boost::proto::eval(boost::proto::as_expr(rhs), ctx);

    vex::generator::get_recorder() << ";\n";
}

enum VarNames {
    V_DEND  ,  // dend.V_dend
    V_SOMA  ,  // soma.V_soma
    V_AXON  ,  // axon.V_axon
    R_D     ,  // dend.Calcium_r
    Z_D     ,  // dend.Potassium_s
    N_D     ,  // dend.Hcurrent_q
    CA_CONC ,  // dend.Ca2Plus
    K_S     ,  // soma.Calcium_k
    L_S     ,  // soma.Calcium_l
    H_S     ,  // soma.Sodium_h
    N_S     ,  // soma.Potassium_n
    X_S     ,  // soma.Potassium_x_s
    H_A     ,  // axon.Sodium_h_a
    X_A        // axon.Potassium_x_a
};

struct nn_system {
    double Cmd = 1;
    double Cms = 1;
    double Cma = 1;

    double g_int = 0.13;
    double g_cah_d = 4.5;
    double g_kca_d = 35;
    double g_h_d = 0.125;
    double g_leak_d = 0.016;
    double g_cal_s = 0.68;
    double g_na_s = 150;
    double g_kdr_s = 9;
    double g_k_s = 5;
    double g_leak_s = 0.016;
    double g_na_a = 240;
    double g_k_a = 20;
    double g_leak_a = 0.016;

    double p1 = 0.25;
    double p2 = 0.15;

    double Vcah_d = 120;
    double Vkca_d = -75;
    double Vh_d = -43;
    double Vleak_d = 10;
    double Vcal_s = 120;
    double Vna_s = 55;
    double Vkdr_s = -75;
    double Vk_s = -75;
    double Vleak_s = 10;
    double Vna_a = 55;
    double Vk_a = -75;
    double Vleak_a = 10;

    void operator()(const sym_state &s, sym_state &dsdt, double t) const {
        using namespace vex;

        double Iapp = 0;

        sym_var v_dend  = s[V_DEND];
        sym_var v_soma  = s[V_SOMA];
        sym_var v_axon  = s[V_AXON];
        sym_var r_d     = s[R_D];
        sym_var z_d     = s[Z_D];
        sym_var n_d     = s[N_D];
        sym_var ca_conc = s[CA_CONC];
        sym_var k_s     = s[K_S];
        sym_var l_s     = s[L_S];
        sym_var h_s     = s[H_S];
        sym_var n_s     = s[N_S];
        sym_var x_s     = s[X_S];
        sym_var h_a     = s[H_A];
        sym_var x_a     = s[X_A];

        auto Igap    = 0;
        auto Isd     = g_int / (1 - p1) * (v_dend - v_soma);
        auto Icah_d  = -g_cah_d * r_d * r_d * (Vcah_d - v_dend);
        auto Ikca_d  = -g_kca_d * z_d * (Vkca_d - v_dend);
        auto Ih_d    = -g_h_d * n_d * (Vh_d - v_dend);
        auto Ileak_d = -g_leak_d * (Vleak_d - v_dend);

        auto Ids     = g_int / p1 * (v_soma - v_dend);
        auto Ias     = g_int / (1 - p2) * (v_soma - v_axon);
        auto Ical_s  = -g_cal_s * k_s * k_s * k_s * l_s * (Vcal_s - v_soma);
        sym_var m_s  = 1 / (1 + exp(-(v_soma + 30) / 5.5));
        auto Ina_s   = -g_na_s * m_s * m_s * m_s * h_s * (Vna_s - v_soma);
        auto Ikdr_s  = -g_kdr_s * n_s * n_s * n_s * n_s * (Vkdr_s - v_soma);
        auto Ik_s    = -g_k_s * x_s * x_s * x_s * x_s * (Vk_s - v_soma);
        auto Ileak_s = -g_leak_s * (Vleak_s - v_soma);

        auto Isa     = g_int / p2 * (v_axon - v_soma);
        sym_var m_a  = 1 / (1 + exp(-(v_axon + 30)/5.5));
        auto Ina_a   = -g_na_a * m_a * m_a * m_a * h_a * (Vna_a - v_axon);
        auto Ik_a    = -g_k_a * x_a * x_a * x_a * x_a * (Vk_a - v_axon);
        auto Ileak_a = -g_leak_a * (Vleak_a - v_axon);

        maxeler_constant_var one_percent(1e-2);

        connect(dsdt[V_DEND ], (-Igap + Iapp - Isd - Icah_d - Ikca_d - Ih_d - Ileak_d) / Cmd);
        connect(dsdt[V_SOMA ], (-Ids - Ias - Ical_s - Ina_s - Ikdr_s - Ik_s - Ileak_s) / Cms);
        connect(dsdt[V_AXON ], (-Isa - Ina_a - Ik_a - Ileak_a) / Cma);
        connect(dsdt[R_D    ], 0.2 * 1.7 / (1 + exp(-(v_dend - 5)/13.9)) * (1 - r_d) -
                        0.2 * 0.1 * (v_dend + 8.5) / (-5) * r_d / (1 - exp((v_dend + 8.5)/5)));
        connect(dsdt[Z_D    ], min(2e-5 * ca_conc, one_percent) * (1 - z_d) - 0.015 * z_d);
        connect(dsdt[N_D    ], (1 / (1 + exp((v_dend + 80) / 4)) - n_d) *
                        (exp(-0.086 * v_dend - 14.6) + exp(0.070 * v_dend - 1.87)));
        connect(dsdt[CA_CONC], -3 * Icah_d - 0.075 * ca_conc);
        connect(dsdt[K_S    ], 1 / (1 + exp(-(v_soma + 61)/4.2)) - k_s);
        connect(dsdt[L_S    ], (1 / (1 + exp((v_soma + 85.5) / 8.5)) - l_s) /
                        ((20 * exp((v_soma + 160) / 30) / (1 + exp((v_soma + 84)/7.3))) + 35));
        connect(dsdt[H_S    ], (1 / (1 + exp((v_soma + 70) / 5.8)) - h_s) /
                        (3 * exp(-(v_soma + 40) / 33)));
        connect(dsdt[N_S    ], (1 / (1 + exp(-(v_soma + 3) / 10)) - n_s) /
                        (5 + 47 * exp((v_soma + 50) / 900)));
        connect(dsdt[X_S    ], (1 - x_s) * (0.13 * (v_soma + 25)) / (1 - exp(-(v_soma + 25) / 10)) -
                        x_s * 1.69 * exp(-(v_soma + 35) / 80));
        connect(dsdt[H_A    ], (1 / (1 + exp((v_axon + 60) / 5.8)) - h_a) /
                        (1.5 * exp(-(v_axon + 40) / 33)));
        connect(dsdt[X_A    ], (1 - x_a) * (0.13 * (v_axon + 25)) / (1 - exp(-(v_axon + 25) / 10)) -
                        x_a * 1.69 * exp(-(v_axon + 35) / 80));
    }
};

//---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    namespace po = boost::program_options;
    po::options_description desc("Options");

    desc.add_options()
        ("help,h", "Show this help.")
        ("size,n",   po::value<int>()->default_value(1152),                 "number of cells to use")
        ("steps,m",  po::value<int>()->default_value(1),                    "number of steps to merge in the kernel")
        ("tmax",     po::value<double>()->default_value(10.0, "10.0"),      "tmax")
        ("tau",      po::value<double>()->default_value(0.05, "0.05"),      "time step")
        ("wstep,w",  po::value<double>()->default_value(0.1,  "0.1"),       "period for writing results")
        ("output,o", po::value<std::string>()->default_value("output.txt"), "output file")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    int         n         = vm["size"].as<int>();
    int         m         = vm["steps"].as<int>();
    std::string out_file  = vm["output"].as<std::string>();
    double      tmax      = vm["tmax"].as<double>();
    double      dt        = vm["tau"].as<double>();
    double      wstep     = vm["wstep"].as<double>();

    vex::Context ctx(vex::Filter::Env && vex::Filter::Count(1));
    std::cout << ctx << std::endl;

    vex::profiler<> prof(ctx);

    nn_system NN;

    // Custom kernel body will be recorded here
    std::ostringstream body;
    vex::generator::set_recorder(body);

    // State types that would become kernel parameters
    sym_state  sym_S{sym_state::VectorParameter};

    odeint::euler<
        sym_state, double, sym_state, double,
        odeint::vector_space_algebra, odeint::default_operations
        > stepper;

    // Record m time steps
    for(int i = 0; i < m; ++i)
        stepper.do_step(NN, sym_S, 0, dt);

    // Generate the kernel from the recorded sequence
    auto kernel = vex::generator::build_kernel(ctx, "nn_ode", body.str(), sym_S);

    // Real state
    vex::vector<double> S(ctx, 14 * n);

    // Initial values
    for(int i = 0, j = 0; i < n; ++i, j += 14) {
        S[j + V_DEND]  = -60;
        S[j + V_SOMA]  = -60;
        S[j + V_AXON]  = -60;
        S[j + R_D]     = 0.0112788;
        S[j + Z_D]     = 0.0049291;
        S[j + N_D]     = 0.0337836;
        S[j + CA_CONC] = 3.7152;
        S[j + K_S]     = 0.7423159;
        S[j + L_S]     = 0.0321349;
        S[j + H_S]     = 0.3596066;
        S[j + N_S]     = 0.2369847;
        S[j + X_S]     = 0.1;
        S[j + H_A]     = 0.9;
        S[j + X_A]     = 0.2369847;
    }

    prof.tic_cl("Solving ODEs");

    prof.tic_cl("load_dfe");
    kernel.load_dfe();
    prof.toc("load_dfe");

    kernel.push_arg(S);
    kernel.push_arg(n);

    std::ofstream out(out_file);
    double chk_point = wstep;

    prof.tic_cl("write_lmem");
    kernel.write_lmem();
    prof.toc("write_lmem");

    prof.tic_cl("integrate");
    for(double t = 0; t < tmax; t += m * dt) {
        kernel.execute();

        if (t >= chk_point) {
            chk_point += wstep;

            prof.tic_cl("save");
            kernel.read_lmem();

            out << t;
            for(int i = 0, j = 0; i < n; ++i, j += 14)
                out << " " << S[j + V_AXON];
            out << std::endl;
            prof.toc("save");
        }
    }
    prof.toc("integrate");

    prof.tic_cl("read_lmem");
    kernel.read_lmem();
    prof.toc("read_lmem");
    prof.toc("Solving ODEs");

    std::cout << prof << std::endl;
}
