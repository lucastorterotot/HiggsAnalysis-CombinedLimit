#include <stdexcept>
#include "HiggsAnalysis/CombinedLimit/interface/Hybrid.h"
#include "RooRealVar.h"
#include "RooArgSet.h"
#include "RooStats/HybridCalculatorOriginal.h"
#include "RooAbsPdf.h"
#include "RooRandom.h"
#include "TFile.h"
#include "TKey.h"
#include "HiggsAnalysis/CombinedLimit/interface/Combine.h"
#include "HiggsAnalysis/CombinedLimit/interface/RooFitGlobalKillSentry.h"

using namespace RooStats;

Hybrid::Hybrid() : 
LimitAlgo("Hybrid specific options") {
    options_.add_options()
        ("toysH,T", boost::program_options::value<unsigned int>(&nToys_)->default_value(500),    "Number of Toy MC extractions to compute CLs+b, CLb and CLs")
        ("clsAcc",  boost::program_options::value<double>(&clsAccuracy_ )->default_value(0.005), "Absolute accuracy on CLs to reach to terminate the scan")
        ("rAbsAcc", boost::program_options::value<double>(&rAbsAccuracy_)->default_value(0.1),   "Absolute accuracy on r to reach to terminate the scan")
        ("rRelAcc", boost::program_options::value<double>(&rRelAccuracy_)->default_value(0.05),  "Relative accuracy on r to reach to terminate the scan")
        ("rule",    boost::program_options::value<std::string>(&rule_)->default_value("CLs"),    "Rule to use: CLs, CLsplusb")
        ("testStat",boost::program_options::value<std::string>(&testStat_)->default_value("LEP"),"Test statistics: LEP, TEV, Atlas.")
        ("rInterval",  "Always try to compute an interval on r even after having found a point satisfiying the CL")
        ("saveHybridResult",  "Save result in the output file  (option saveToys must be enabled)")
        ("readHybridResults", "Read and merge results from file (option toysFile must be enabled)")
    ;
}

void Hybrid::applyOptions(const boost::program_options::variables_map &vm) {
    if (rule_ == "CLs") {
        CLs_ = true;
    } else if (rule_ == "CLsplusb") {
        CLs_ = false;
    } else {
        throw std::invalid_argument("Hybrid: Rule should be one of 'CLs' or 'CLsplusb'");
    }
    rInterval_ = vm.count("rInterval");
    if (testStat_ != "LEP" && testStat_ != "TEV" && testStat_ != "Atlas") {
        throw std::invalid_argument("Hybrid: Test statistics should be one of 'LEP' or 'TEV' or 'Atlas'");
    }
    saveHybridResult_ = vm.count("saveHybridResult");
    readHybridResults_ = vm.count("readHybridResults");
}

bool Hybrid::run(RooWorkspace *w, RooAbsData &data, double &limit, const double *hint) {
  RooFitGlobalKillSentry silence(RooFit::WARNING);
  RooRealVar *r = w->var("r"); r->setConstant(true);
  RooArgSet  poi(*r);
  w->loadSnapshot("clean");
  RooAbsPdf *altModel  = w->pdf("model_s"), *nullModel = w->pdf("model_b");
  
  HybridCalculatorOriginal* hc = new HybridCalculatorOriginal(data,*altModel,*nullModel);
  if (withSystematics) {
    if ((w->set("nuisances") == 0) || (w->pdf("nuisancePdf") == 0)) {
      throw std::logic_error("Hybrid: running with systematics enabled, but nuisances or nuisancePdf not defined.");
    }
    hc->UseNuisance(true);
    hc->SetNuisancePdf(*w->pdf("nuisancePdf"));
    hc->SetNuisanceParameters(*w->set("nuisances"));
  } else {
    hc->UseNuisance(false);
  }
  if (testStat_ == "LEP") {
    hc->SetTestStatistic(1);
    r->setConstant(true);
  } else if (testStat_ == "TEV") {
    hc->SetTestStatistic(3);
    r->setConstant(true);
  } else if (testStat_ == "Atlas") {
    hc->SetTestStatistic(3);
    r->setConstant(false);
  }
  hc->PatchSetExtended(w->pdf("model_b")->canBeExtended()); // Number counting, each dataset has 1 entry 
  hc->SetNumberOfToys(nToys_);

  return doSignificance_ ? runSignificance(hc, w, data, limit, hint) : runLimit(hc, w, data, limit, hint);
}
  
bool Hybrid::runLimit(HybridCalculatorOriginal* hc, RooWorkspace *w, RooAbsData &data, double &limit, const double *hint) {
  RooRealVar *r = w->var("r"); r->setConstant(true);
  if ((hint != 0) && (*hint > r->getMin())) {
    r->setMax(std::min<double>(3*(*hint), r->getMax()));
  }
  
  typedef std::pair<double,double> CLs_t;

  double clsTarget = 1 - cl; 
  CLs_t clsMin(1,0), clsMax(0,0);
  double rMin = 0, rMax = r->getMax();

  std::cout << "Search for upper limit to the limit" << std::endl;
  for (;;) {
    CLs_t clsMax = eval(r, r->getMax(), hc);
    if (clsMax.first == 0 || clsMax.first + 3 * fabs(clsMax.second) < cl ) break;
    r->setMax(r->getMax()*2);
    if (r->getVal()/rMax >= 20) { 
      std::cerr << "Cannot set higher limit: at r = " << r->getVal() << " still get " << (CLs_ ? "CLs" : "CLsplusb") << " = " << clsMax.first << std::endl;
      return false;
    }
  }
  rMax = r->getMax();
  
  std::cout << "Now doing proper bracketing & bisection" << std::endl;
  bool lucky = false;
  do {
    CLs_t clsMid = eval(r, 0.5*(rMin+rMax), hc, true, clsTarget);
    if (clsMid.second == -1) {
      std::cerr << "Hypotest failed" << std::endl;
      return false;
    }
    if (fabs(clsMid.first-clsTarget) <= clsAccuracy_) {
      std::cout << "reached accuracy." << std::endl;
      lucky = true;
      break;
    }
    if ((clsMid.first>clsTarget) == (clsMax.first>clsTarget)) {
      rMax = r->getVal(); clsMax = clsMid;
    } else {
      rMin = r->getVal(); clsMin = clsMid;
    }
  } while (rMax-rMin > std::max(rAbsAccuracy_, rRelAccuracy_ * r->getVal()));

  if (lucky) {
    limit = r->getVal();
    if (rInterval_) {
      std::cout << "\n -- HypoTestInverter (before determining interval) -- \n";
      std::cout << "Limit: r < " << limit << " +/- " << 0.5*(rMax - rMin) << " @ " <<cl * 100<<"% CL\n";

      double rBoundLow  = limit - 0.5*std::max(rAbsAccuracy_, rRelAccuracy_ * limit);
      for (r->setVal(rMin); r->getVal() < rBoundLow  && (fabs(clsMin.first-clsTarget) >= clsAccuracy_); rMin = r->getVal()) {
        clsMax = eval(r, 0.5*(r->getVal()+limit), hc, true, clsTarget);
      }

      double rBoundHigh = limit + 0.5*std::max(rAbsAccuracy_, rRelAccuracy_ * limit);
      for (r->setVal(rMax); r->getVal() > rBoundHigh && (fabs(clsMax.first-clsTarget) >= clsAccuracy_); rMax = r->getVal()) {
        clsMax = eval(r, 0.5*(r->getVal()+limit), hc, true, clsTarget);
      }
    }
  } else {
    limit = 0.5*(rMax+rMin);
  }
  std::cout << "\n -- HypoTestInverter -- \n";
  std::cout << "Limit: r < " << limit << " +/- " << 0.5*(rMax - rMin) << " @ " <<cl * 100<<"% CL\n";
  return true;
}

bool Hybrid::runSignificance(HybridCalculatorOriginal* hc, RooWorkspace *w, RooAbsData &data, double &limit, const double *hint) {
    using namespace RooStats;
    RooRealVar *r = w->var("r"); 
    r->setVal(1);
    r->setConstant(true);
    std::auto_ptr<HybridResult> hcResult(readHybridResults_ ? readToysFromFile() : hc->GetHypoTest());
    if (hcResult.get() == 0) {
        std::cerr << "Hypotest failed" << std::endl;
        return false;
    }
    if (saveHybridResult_) {
        if (writeToysHere == 0) throw std::logic_error("Option saveToys must be enabled to turn on saveHybridResult");
        TString name = TString::Format("HybridResult_%u", RooRandom::integer(std::numeric_limits<UInt_t>::max() - 1));
        writeToysHere->WriteTObject(new HybridResult(*hcResult), name);
        if (verbose) std::cout << "Hybrid result saved as " << name << " in " << writeToysHere->GetFile()->GetName() << " : " << writeToysHere->GetPath() << std::endl;
    }
    limit = hcResult->Significance();
    double sigHi = RooStats::PValueToSignificance( 1 - (hcResult->CLb() + hcResult->CLbError()) ) - limit;
    double sigLo = RooStats::PValueToSignificance( 1 - (hcResult->CLb() - hcResult->CLbError()) ) - limit;
    std::cout << "\n -- Hybrid -- \n";
    std::cout << "Significance: " << limit << "  " << sigLo << "/+" << sigHi << " (CLb " << hcResult->CLb() << " +/- " << hcResult->CLbError() << ")\n";
    return isfinite(limit);
}

RooStats::HybridResult * Hybrid::readToysFromFile() {
    if (!readToysFromHere) throw std::logic_error("Cannot use readHybridResult: option toysFile not specified, or input file empty");
    TDirectory *toyDir = readToysFromHere->GetDirectory("toys");
    if (!toyDir) throw std::logic_error("Cannot use readHybridResult: option toysFile not specified, or input file empty");
    if (verbose) std::cout << "Reading toys" << std::endl;

    std::auto_ptr<RooStats::HybridResult> ret;
    TIter next(toyDir->GetListOfKeys()); TKey *k;
    while ((k = (TKey *) next()) != 0) {
        if (TString(k->GetName()).Index("HybridResult_") != 0) continue;
        RooStats::HybridResult *toy = dynamic_cast<RooStats::HybridResult *>(toyDir->Get(k->GetName()));
        if (toy == 0) continue;
        if (verbose) std::cout << " - " << k->GetName() << std::endl;
        if (ret.get() == 0) {
            ret.reset(new RooStats::HybridResult(*toy));
        } else {
            ret->Append(toy);
        }
    }

    return ret.release();
}
std::pair<double, double> Hybrid::eval(RooRealVar *r, double rVal, RooStats::HybridCalculatorOriginal *hc, bool adaptive, double clsTarget) {
    using namespace RooStats;
    r->setVal(rVal);
    std::auto_ptr<HybridResult> hcResult(hc->GetHypoTest());
    if (hcResult.get() == 0) {
        std::cerr << "Hypotest failed" << std::endl;
        return std::pair<double, double>(-1,-1);
    }
    double clsMid    = (CLs_ ? hcResult->CLs()      : hcResult->CLsplusb());
    double clsMidErr = (CLs_ ? hcResult->CLsError() : hcResult->CLsplusbError());
    std::cout << "r = " << rVal << (CLs_ ? ": CLs = " : ": CLsplusb = ") << clsMid << " +/- " << clsMidErr << std::endl;
    if (adaptive) {
        while (fabs(clsMid-clsTarget) < 3*clsMidErr && clsMidErr >= clsAccuracy_) {
            std::auto_ptr<HybridResult> more(hc->GetHypoTest());
            hcResult->Add(more.get());
            clsMid    = (CLs_ ? hcResult->CLs()      : hcResult->CLsplusb());
            clsMidErr = (CLs_ ? hcResult->CLsError() : hcResult->CLsplusbError());
            std::cout << "r = " << rVal << (CLs_ ? ": CLs = " : ": CLsplusb = ") << clsMid << " +/- " << clsMidErr << std::endl;
        }
    }
    if (verbose > 0) {
        std::cout << "r = " << r->getVal() << ": \n" <<
            "\tCLs      = " << hcResult->CLs()      << " +/- " << hcResult->CLsError()      << "\n" <<
            "\tCLb      = " << hcResult->CLb()      << " +/- " << hcResult->CLbError()      << "\n" <<
            "\tCLsplusb = " << hcResult->CLsplusb() << " +/- " << hcResult->CLsplusbError() << "\n" <<
            std::endl;
    }
    return std::pair<double, double>(clsMid, clsMidErr);
} 
