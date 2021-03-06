/*
   Copyright (c) by respective owners including Yahoo!, Microsoft, and
   individual contributors. All rights reserved.  Released under a BSD (revised)
   license as described in the file LICENSE.
   */
#include <fstream>
#include <float.h>
#ifndef _WIN32
#include <netdb.h>
#endif
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/timeb.h>
#include "parse_example.h"
#include "constant.h"
#include "cache.h"
#include "simple_label.h"
#include "vw.h"
#include "gd.h"
#include "accumulate.h"
#include "memory.h"
#include <exception>

using namespace std;
using namespace LEARNER;


#define W_XT 0   // current parameter w(XT)
#define W_GT 1   // current gradient  g(GT)
#define W_ZT 2   // accumulated z(t) = z(t-1) + g(t) + sigma*w(t)
#define W_G2 3   // accumulated gradient squre n(t) = n(t-1) + g(t)*g(t)

/********************************************************************/
/* mem & w definition ***********************************************/
/********************************************************************/ 
// w[0] = current weight
// w[1] = current first derivative
// w[2] = accumulated zt
// w[3] = accumulated g2

namespace FTRL {

  //nonrentrant
  struct ftrl {

    vw* all;
    // set by initializer
    float ftrl_alpha;
    float ftrl_beta;

    // evaluation file pointer
    FILE* fo;
    bool progressive_validation;
  };
  
  void update_accumulated_state(weight* w, float ftrl_alpha) {
    double ng2 = w[W_G2] + w[W_GT]*w[W_GT];
    double sigma = (sqrt(ng2) - sqrt(w[W_G2]))/ ftrl_alpha;
    w[W_ZT] += w[W_GT] - sigma * w[W_XT];
    w[W_G2] = ng2;
  }

  struct update_data {
    float update;
    float ftrl_alpha;
    float ftrl_beta;
    float l1_lambda;
    float l2_lambda;
   };
   
  //void update_grad(weight* weights, size_t mask, float loss_grad)
  void update_grad(update_data& d, float x, float& wref) {
        float* w = &wref;
        w[W_GT] = d.update * x;
        update_accumulated_state(w, d.ftrl_alpha);
   }

  float ftrl_predict(vw& all, example& ec) {
    ec.partial_prediction = GD::inline_predict(all, ec);
    return GD::finalize_prediction(all.sd, ec.partial_prediction);
  }

  float predict_and_gradient(vw& all, ftrl &b, example& ec) {
    float fp = ftrl_predict(all, ec);
    ec.updated_prediction = fp;

    label_data& ld = ec.l.simple;
    all.set_minmax(all.sd, ld.label);

    struct update_data data;
    
    data.update = all.loss->first_derivative(all.sd, fp, ld.label) * ld.weight;
    data.ftrl_alpha = b.ftrl_alpha;
    
    GD::foreach_feature<update_data,update_grad>(all, ec, data);

    return fp;
  }

 inline float sign(float w){ if (w < 0.) return -1.; else  return 1.;}

 void update_w(update_data& d, float x, float& wref) {
    float* w = &wref;
    float flag = sign(w[W_ZT]);
    float fabs_zt = w[W_ZT] * flag;
    if (fabs_zt <= d.l1_lambda) {
      w[W_XT] = 0.;
    } else {
      double step = 1/(d.l2_lambda + (d.ftrl_beta + sqrt(w[W_G2]))/d.ftrl_alpha);
      w[W_XT] = step * flag * (d.l1_lambda - fabs_zt);
    }
 }
 
  void update_weight(vw& all, ftrl &b, example& ec) {
      
    struct update_data data;
    
    data.ftrl_alpha = b.ftrl_alpha;
    data.ftrl_beta = b.ftrl_beta;
    data.l1_lambda = all.l1_lambda;
    data.l2_lambda = all.l2_lambda;
      
    GD::foreach_feature<update_data, update_w>(all, ec, data);

  }

  void evaluate_example(vw& all, ftrl& b , example& ec) {
    label_data& ld = ec.l.simple;
    ec.loss = all.loss->getLoss(all.sd, ec.updated_prediction, ld.label) * ld.weight;
    if (b.progressive_validation) {
      float v = 1./(1 + exp(-ec.updated_prediction));
      fprintf(b.fo, "%.6f\t%d\n", v, (int)(ld.label * ld.weight));
    }
  }

  //void learn(void* a, void* d, example* ec) {
  void learn(ftrl& a, learner& base, example& ec) {
    vw* all = a.all;
    assert(ec.in_use);
 
    // predict w*x, compute gradient, update accumulate state
    predict_and_gradient(*all, a, ec);
    // evaluate, statistic
    evaluate_example(*all, a, ec);
    // update weight
    update_weight(*all, a, ec);
  }

  void save_load(ftrl& b, io_buf& model_file, bool read, bool text) {
    vw* all = b.all;
    if (read) {
      initialize_regressor(*all);
    } 

    if (model_file.files.size() > 0) {
      bool resume = all->save_resume;
      char buff[512];
      uint32_t text_len = sprintf(buff, ":%d\n", resume);
      bin_text_read_write_fixed(model_file,(char *)&resume, sizeof (resume), "", read, buff, text_len, text);

      if (resume) {
        GD::save_load_online_state(*all, model_file, read, text);
        //save_load_online_state(*all, model_file, read, text);
      } else {
        GD::save_load_regressor(*all, model_file, read, text);
      }
    }

  }
  
  // placeholder
  void predict(ftrl& b, learner& base, example& ec)
  {
    vw* all = b.all;
    //ec.l.simple.prediction = ftrl_predict(*all,ec);
    ec.pred.scalar = ftrl_predict(*all,ec);
  }
  
  learner* setup(vw& all, po::variables_map& vm) {

    ftrl* b = (ftrl*)calloc_or_die(1, sizeof(ftrl));
    b->all = &all;
    b->ftrl_beta = 0.0;
    b->ftrl_alpha = 0.1;

    po::options_description ftrl_opts("FTRL options");

    ftrl_opts.add_options()
                ("ftrl_alpha", po::value<float>(&(b->ftrl_alpha)), "Learning rate for FTRL-proximal optimization")
                ("ftrl_beta", po::value<float>(&(b->ftrl_beta)), "FTRL beta")
                ("progressive_validation", po::value<string>()->default_value("ftrl.evl"), "File to record progressive validation for ftrl-proximal");

    vm = add_options(all, ftrl_opts);

    if (vm.count("ftrl_alpha")) {
        b->ftrl_alpha = vm["ftrl_alpha"].as<float>();
    }

    if (vm.count("ftrl_beta")) {
        b->ftrl_beta = vm["ftrl_beta"].as<float>();
    }

    all.reg.stride_shift = 2; // NOTE: for more parameter storage
    
    b->progressive_validation = false;
    if (vm.count("progressive_validation")) {
      std::string filename = vm["progressive_validation"].as<string>();
      b->fo = fopen(filename.c_str(), "w");
      assert(b->fo != NULL);
      b->progressive_validation = true;
    }

    if (!all.quiet) {
        cerr << "Enabling FTRL-Proximal based optimization" << endl;
        cerr << "ftrl_alpha = " << b->ftrl_alpha << endl;
        cerr << "ftrl_beta = " << b->ftrl_beta << endl;
    }

    learner* l = new learner(b, 1 << all.reg.stride_shift);
    l->set_learn<ftrl, learn>();
    l->set_predict<ftrl, predict>();
    l->set_save_load<ftrl,save_load>();

    return l;
  }


} // end namespace
