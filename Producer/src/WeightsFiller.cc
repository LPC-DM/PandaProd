#include "../interface/WeightsFiller.h"

#include "FWCore/Framework/interface/Run.h"

#include "PandaTree/Framework/interface/IOUtils.h"

#include "TDOMParser.h"
#include "TXMLNode.h"
#include "TXMLAttr.h"

auto GetAll([](edm::BranchDescription const&)->bool { return true; });

WeightsFiller::WeightsFiller(std::string const& _name, edm::ParameterSet const& _cfg, edm::ConsumesCollector& _coll) :
  FillerBase(_name, _cfg)
{
  if (!isRealData_) {
    getToken_(genInfoToken_, _cfg, _coll, "common", "genEventInfo");
    // Some samples have non-standard LHEEventProduct names
    // Using notifyNewProduct() to dynamically find the tag
    lheEventToken_.first = "lheEvent";
  }
}

void
WeightsFiller::branchNames(panda::utils::BranchList& _eventBranches, panda::utils::BranchList&) const
{
  _eventBranches.emplace_back("weight");
  if (!isRealData_) {
    _eventBranches.emplace_back("genReweight");
    // genParam is booked at the first encounter
    _eventBranches.push_back("!genReweight.genParam");
  }
}

void
WeightsFiller::addOutput(TFile& _outputFile)
{
  if (isRealData_)
    hSumW_ = new TH1D("hSumW", "SumW", 1, 0., 1.);
  else
    hSumW_ = new TH1D("hSumW", "SumW", 8, 0., 8.);

  hSumW_->SetDirectory(&_outputFile);

  hSumW_->GetXaxis()->SetBinLabel(1, "Nominal");

  if (!isRealData_) {
    char const* labels[] = {
      "r1f2",
      "r1f5",
      "r2f1",
      "r2f2",
      "r5f1",
      "r5f5",
      "pdf"
    };

    for (unsigned iL(0); iL != sizeof(labels) / sizeof(char const*); ++iL)
      hSumW_->GetXaxis()->SetBinLabel(iL + 2, labels[iL]);

    outputFile_ = &_outputFile;
  }
}

void
WeightsFiller::fillAll(edm::Event const& _inEvent, edm::EventSetup const&)
{
  if (isRealData_) {
    hSumW_->Fill(0.5);
    return;
  }

  auto& genInfo(getProduct_(_inEvent, genInfoToken_));
  central_ = genInfo.weight();

  hSumW_->Fill(0.5, central_);

  if (lheEventToken_.second.isUninitialized())
    return;

  auto& lheEvent(getProduct_(_inEvent, lheEventToken_));
  getLHEWeights_(lheEvent);

  // PDF variation will always be greater than nominal by construction
  for (unsigned iW(0); iW != 7; ++iW)
    hSumW_->Fill(iW + 1.5, normQCDVariations_[iW] * central_);

  for (unsigned iS(0); iS != wids_.size(); ++iS) {
    if (genParam_[iS] >= 0.)
      hSumW_->Fill(iS + 9.5, genParam_[iS] * central_);
  }
}

void
WeightsFiller::fill(panda::Event& _outEvent, edm::Event const&, edm::EventSetup const&)
{
  // fillAll is always called before fill

  if (isRealData_) {
    _outEvent.weight = 1.;
    return;
  }

  _outEvent.weight = central_;

  if (bufferCounter_ == 0) // getLHEWeights was not called
    return;

  // Save the offset of normalized reweight factor from 1 for precision
  // (Normalized weights have values close to 1, and epsilon can be saved with higher precision than 1 + epsilon)
  _outEvent.genReweight.r1f2DW = normQCDVariations_[0] - 1.;
  _outEvent.genReweight.r1f5DW = normQCDVariations_[1] - 1.;
  _outEvent.genReweight.r2f1DW = normQCDVariations_[2] - 1.;
  _outEvent.genReweight.r2f2DW = normQCDVariations_[3] - 1.;
  _outEvent.genReweight.r5f1DW = normQCDVariations_[4] - 1.;
  _outEvent.genReweight.r5f5DW = normQCDVariations_[5] - 1.;
  _outEvent.genReweight.pdfDW = normQCDVariations_[6] - 1.;

  // genParam branch is not filled from the outEvent object, but we copy the value here for consistence
  // (some other filler module may decide to use the values!)
  // Unlike QCD variation reweights, genParam can represent anything and is not guaranteed to cluster around 1.
  // Therefore we save the normalized weights directly and do not subtract 1.
  std::copy(genParam_, genParam_ + wids_.size(), _outEvent.genReweight.genParam);
}

void
WeightsFiller::fillEndRun(panda::Run&, edm::Run const&, edm::EventSetup const&)
{
  if (!isRealData_ && bufferCounter_ < learningPhase) {
    // Run boundary before getting out of learning phase
    // It could be a genuine run boundary, but there is no way to tell -> we need to exit learning phase now
    bookGenParam_();

    bufferCounter_ = 0xffffffff;
  }
}

void
WeightsFiller::notifyNewProduct(edm::BranchDescription const& _bdesc, edm::ConsumesCollector& _coll)
{
  // assumption: there should be at most one LHEEventProduct in an event
  if (_bdesc.unwrappedTypeID() == edm::TypeID(typeid(LHEEventProduct))) {
    edm::InputTag tag(_bdesc.moduleLabel(), _bdesc.productInstanceName(), _bdesc.processName());
    lheEventToken_.second = _coll.consumes<LHEEventProduct>(tag);
  }
}

void
WeightsFiller::getLHEWeights_(LHEEventProduct const& _lheEvent)
{
  // Update this function if changing the set of weights to save

  double sumd2(0.);
  unsigned iS(0);
  std::fill_n(genParam_, sizeof(genParam_) / sizeof(float), -1.);

  // this is not the same as central_ in MadGraph (LO) samples
  double lheCentral(_lheEvent.originalXWGTUP());

  for (auto& wgt : _lheEvent.weights()) {
    unsigned id(0);
    try {
      id = std::stoi(wgt.id);
    }
    catch (std::invalid_argument& ex) {
      // assumption: this is signal reweights

      if (iS >= wids_.size()) {
        if (bufferCounter_ < learningPhase) {
          // assumption: weights always come in the same order, but the list can be truncated
          wids_.emplace_back(wgt.id);

          unsigned nbinsx(hSumW_->GetNbinsX() + 1);
          hSumW_->SetBins(nbinsx, 0., nbinsx);
          hSumW_->GetXaxis()->SetBinLabel(nbinsx, wgt.id.c_str());
        }
        else {
          std::cerr << "Found more signal weights after the learning phase - cannot handle it." << std::endl;
          continue;
        }
      }

      // unlike QCD weights, we simply save normalized weights to the tree
      genParam_[iS++] = wgt.wgt / lheCentral;

      continue;
    }

    if ((id >= 1 && id <= 9) || (id >= 1001 && id <= 1009)) {
      unsigned iV(0);
      switch (id % 1000) {
      case 2:
        iV = 0;
        break;
      case 3:
        iV = 1;
        break;
      case 4:
        iV = 2;
        break;
      case 5:
        iV = 3;
        break;
      case 7:
        iV = 4;
        break;
      case 9:
        iV = 5;
        break;
      default:
        continue; // r2f5 and r5f2 -> won't save
      }

      normQCDVariations_[iV] = wgt.wgt / lheCentral;
    }
    else if ((id >= 11 && id <= 110) || (id >= 2001 && id < 2100)) {
      // We assume NNPDF-type MC uncertainties (as opposed to CTEQ-type fit correlation matrix eigenvalues)
      // Reference: https://nnpdf.hepforge.org/html/tutorial.html
      // sigma = sqrt[sum_{i=1..100}[(w_i - w_0)^2] / 99]
      double d(wgt.wgt - lheCentral);
      sumd2 += d * d;
    }
  }

  // We fill the sumW histogram with (1 + sigma / w_0), and save sigma / w_0 in the trees
  normQCDVariations_[6] = std::sqrt(sumd2 / 99.) / lheCentral + 1.;

  if (bufferCounter_ < learningPhase) {
    // save the weights to the buffer
    std::copy(genParam_, genParam_ + iS, genParamBuffer_[bufferCounter_]);
    ++bufferCounter_;
  }
  else if (bufferCounter_ == learningPhase) {
    // By now we should know how large the signal weights vector is
    bookGenParam_();
    bufferCounter_ = 0xffffffff;
  }
}

void
WeightsFiller::bookGenParam_()
{
  if (wids_.size() == 0)
    return;

  TDirectory::TContext context(outputFile_);

  auto* weightTree(new TTree("weights", "weights"));
  auto* wid(new TString);
  weightTree->Branch("id", "TString", &wid); // currently only have the ability to save ID

  for (TString& w : wids_) {
    *wid = w;
    weightTree->Fill();
  }

  auto* eventTree(static_cast<TTree*>(outputFile_->Get("events")));
  auto* branch(eventTree->Branch("genReweight.genParam", genParam_, TString::Format("genParam[%d]/F", int(wids_.size()))));

  for (unsigned iE(0); iE != learningPhase; ++iE) {
    std::copy(genParamBuffer_[iE], genParamBuffer_[iE] + wids_.size(), genParam_);
    branch->Fill();
  }
}

DEFINE_TREEFILLER(WeightsFiller);
