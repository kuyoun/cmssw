/*
 * RecoTauProducer
 *
 * Interface between the various tau algorithms and the edm::Event.  The
 * RecoTauProducer takes as data input is a collection (view) of reco::PFJets,
 * and Jet-PiZero assoications that give the reco::RecoTauPiZeros for those
 * jets.  The actual building of taus is done by the list of builders - each of
 * which constructs a PFTau for each PFJet.  The output collection may have
 * multiple taus for each PFJet - these overlaps are to be resolved by the
 * RecoTauCleaner module.
 *
 * Additionally, there are "modifier" plugins, which can do things like add the
 * lead track significance, or electron rejection variables.
 *
 * Authors: Evan K. Friis (UC Davis),
 *          Christian Veelken (LLR)
 *
 */
#include "boost/bind.hpp"
#include <boost/ptr_container/ptr_vector.hpp>

#include <algorithm>
#include <functional>

#include "FWCore/Framework/interface/stream/EDProducer.h"
#include "FWCore/Framework/interface/EventSetup.h"
#include "FWCore/Framework/interface/ESHandle.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include <FWCore/ParameterSet/interface/ConfigurationDescriptions.h>
#include <FWCore/ParameterSet/interface/ParameterSetDescription.h>

#include "RecoTauTag/RecoTau/interface/RecoTauBuilderPlugins.h"
#include "RecoTauTag/RecoTau/interface/RecoTauCommonUtilities.h"

#include "DataFormats/JetReco/interface/PFJetCollection.h"
#include "DataFormats/TauReco/interface/PFJetChargedHadronAssociation.h"
#include "DataFormats/TauReco/interface/PFRecoTauChargedHadron.h"
#include "DataFormats/TauReco/interface/JetPiZeroAssociation.h"
#include "DataFormats/TauReco/interface/PFTau.h"
#include "DataFormats/JetReco/interface/JetCollection.h"
#include "DataFormats/Common/interface/Association.h"

#include "CommonTools/Utils/interface/StringCutObjectSelector.h"

class RecoTauProducer : public edm::stream::EDProducer<> 
{
 public:
  typedef reco::tau::RecoTauBuilderPlugin Builder;
  typedef reco::tau::RecoTauModifierPlugin Modifier;
  typedef std::vector<std::unique_ptr<Builder>> BuilderList;
  typedef std::vector<std::unique_ptr<Modifier>> ModifierList;

  explicit RecoTauProducer(const edm::ParameterSet& pset);
  ~RecoTauProducer() override {}
  void produce(edm::Event& evt, const edm::EventSetup& es) override;

  static void fillDescriptions(edm::ConfigurationDescriptions & descriptions);

 private:
  edm::InputTag jetSrc_;
  edm::InputTag jetRegionSrc_;
  edm::InputTag chargedHadronSrc_;
  edm::InputTag piZeroSrc_;

  double minJetPt_;
  double maxJetAbsEta_;
 //token definition
  edm::EDGetTokenT<reco::JetView> jet_token;
  edm::EDGetTokenT<edm::AssociationMap<edm::OneToOne<reco::JetView, reco::JetView> > > jetRegion_token;
  edm::EDGetTokenT<reco::PFJetChargedHadronAssociation> chargedHadron_token;
  edm::EDGetTokenT<reco::JetPiZeroAssociation> piZero_token;

  BuilderList builders_;
  ModifierList modifiers_;
  // Optional selection on the output of the taus
  std::unique_ptr<StringCutObjectSelector<reco::PFTau> > outputSelector_;
  // Whether or not to add build a tau from a jet for which the builders
  // return no taus.  The tau will have no content, only the four vector of
  // the orginal jet.
  bool buildNullTaus_;
};

RecoTauProducer::RecoTauProducer(const edm::ParameterSet& pset) 
{
  jetSrc_ = pset.getParameter<edm::InputTag>("jetSrc");
  jetRegionSrc_ = pset.getParameter<edm::InputTag>("jetRegionSrc");
  chargedHadronSrc_ = pset.getParameter<edm::InputTag>("chargedHadronSrc");
  piZeroSrc_ = pset.getParameter<edm::InputTag>("piZeroSrc");
  
  minJetPt_ = pset.getParameter<double>("minJetPt");
  maxJetAbsEta_ = pset.getParameter<double>("maxJetAbsEta");
  //consumes definition
  jet_token=consumes<reco::JetView>(jetSrc_);
  jetRegion_token = consumes<edm::AssociationMap<edm::OneToOne<reco::JetView, reco::JetView> > >(jetRegionSrc_);
  chargedHadron_token = consumes<reco::PFJetChargedHadronAssociation>(chargedHadronSrc_); 
  piZero_token = consumes<reco::JetPiZeroAssociation>(piZeroSrc_);

  typedef std::vector<edm::ParameterSet> VPSet;
  // Get each of our tau builders
  const VPSet& builders = pset.getParameter<VPSet>("builders");
  for ( VPSet::const_iterator builderPSet = builders.begin();
	builderPSet != builders.end(); ++builderPSet ) {
    // Get plugin name
    const std::string& pluginType = builderPSet->getParameter<std::string>("plugin");
    // Build the plugin
    builders_.emplace_back(RecoTauBuilderPluginFactory::get()->create(pluginType, *builderPSet, consumesCollector()));
  }

  const VPSet& modfiers = pset.getParameter<VPSet>("modifiers");
  for ( VPSet::const_iterator modfierPSet = modfiers.begin();
	modfierPSet != modfiers.end(); ++modfierPSet) {
    // Get plugin name
    const std::string& pluginType = modfierPSet->getParameter<std::string>("plugin");
    // Build the plugin
    modifiers_.emplace_back(RecoTauModifierPluginFactory::get()->create(pluginType, *modfierPSet, consumesCollector()));
  }

  // Check if we want to apply a final output selection
  std::string selection = pset.getParameter<std::string>("outputSelection");
  if ( !selection.empty() ) {
    outputSelector_.reset(new StringCutObjectSelector<reco::PFTau>(selection));
  }
  buildNullTaus_ = pset.getParameter<bool>("buildNullTaus");

  produces<reco::PFTauCollection>();
}

void RecoTauProducer::produce(edm::Event& evt, const edm::EventSetup& es) 
{
  // Get the jet input collection via a view of Candidates
  edm::Handle<reco::JetView> jetView;
  evt.getByToken(jet_token, jetView);
    
  // Get the jet region producer
  edm::Handle<edm::AssociationMap<edm::OneToOne<reco::JetView, reco::JetView> > > jetRegionHandle;
  evt.getByToken(jetRegion_token, jetRegionHandle);
  
  // Get the charged hadron input collection
  edm::Handle<reco::PFJetChargedHadronAssociation> chargedHadronAssoc;
  evt.getByToken(chargedHadron_token, chargedHadronAssoc);

  // Get the pizero input collection
  edm::Handle<reco::JetPiZeroAssociation> piZeroAssoc;
  evt.getByToken(piZero_token, piZeroAssoc);

  // Update all our builders and modifiers with the event info
  for (auto& builder: builders_) {
    builder->setup(evt, es);
  }
  for (auto& modifier: modifiers_) {
    modifier->setup(evt, es);
  }

  // Create output collection
  auto output = std::make_unique<reco::PFTauCollection>();
  output->reserve(jetView->size());
  
  // Loop over the jets and build the taus for each jet
  for (size_t i_j = 0; i_j < jetView->size(); ++i_j) {
    const auto& jetRef = jetView->refAt(i_j);
    // Get the jet with extra constituents from an area around the jet
    if(jetRef->pt() - minJetPt_ < 1e-5) continue;
    if(std::abs(jetRef->eta()) - maxJetAbsEta_ > -1e-5) continue;
    reco::JetBaseRef jetRegionRef = (*jetRegionHandle)[jetRef];
    if ( jetRegionRef.isNull() ) {
      throw cms::Exception("BadJetRegionRef") 
	<< "No jet region can be found for the current jet: " << jetRef.id();
    }
    // Remove all the jet constituents from the jet extras
    std::vector<reco::CandidatePtr> jetCands = jetRef->daughterPtrVector();
    std::vector<reco::CandidatePtr> allRegionalCands = jetRegionRef->daughterPtrVector();
    // Sort both by ref key
    std::sort(jetCands.begin(), jetCands.end());
    std::sort(allRegionalCands.begin(), allRegionalCands.end());
    // Get the regional junk candidates not in the jet.
    std::vector<reco::CandidatePtr> uniqueRegionalCands;

    // This can actually be less than zero, if the jet has really crazy soft
    // stuff really far away from the jet axis.
    if ( allRegionalCands.size() > jetCands.size() ) {
      uniqueRegionalCands.reserve(allRegionalCands.size() - jetCands.size());
    }

    // Subtract the jet cands from the regional cands
    std::set_difference(allRegionalCands.begin(), allRegionalCands.end(),
			jetCands.begin(), jetCands.end(),
			std::back_inserter(uniqueRegionalCands));

    // Get the charged hadrons associated with this jet
    const std::vector<reco::PFRecoTauChargedHadron>& chargedHadrons = (*chargedHadronAssoc)[jetRef];

    // Get the pizeros associated with this jet
    const std::vector<reco::RecoTauPiZero>& piZeros = (*piZeroAssoc)[jetRef];
    // Loop over our builders and create the set of taus for this jet
    unsigned int nTausBuilt = 0;
    for ( const auto& builder: builders_ ) {
      // Get a ptr_vector of taus from the builder
      reco::tau::RecoTauBuilderPlugin::output_type taus((*builder)(jetRef, chargedHadrons, piZeros, uniqueRegionalCands));

      // Make sure all taus have their jetref set correctly
      std::for_each(taus.begin(), taus.end(), boost::bind(&reco::PFTau::setjetRef, _1, reco::JetBaseRef(jetRef)));
      // Copy without selection
      if ( !outputSelector_.get() ) {
        output->insert(output->end(), taus.begin(), taus.end());
        nTausBuilt += taus.size();
      } else {
        // Copy only those that pass the selection.
        for(auto const& tau : taus ) {
          if ( (*outputSelector_)(tau) ) {
            nTausBuilt++;
            output->push_back(tau);
          }
        }
      }
    }
    // If we didn't build *any* taus for this jet, build a null tau if desired.
    // The null PFTau has no content, but it's four vector is set to that of the
    // jet.
    if ( !nTausBuilt && buildNullTaus_ ) {
      reco::PFTau nullTau(std::numeric_limits<int>::quiet_NaN(), jetRef->p4());
      nullTau.setjetRef(reco::JetBaseRef(jetRef));
      output->push_back(nullTau);
    }
  }

  // Loop over the taus we have created and apply our modifiers to the taus
  for ( reco::PFTauCollection::iterator tau = output->begin();
	tau != output->end(); ++tau ) {
    for ( const auto& modifier: modifiers_ ) {
      (*modifier)(*tau);
    }
  }
  
  for ( auto& modifier: modifiers_ ) {
    modifier->endEvent();
  }
  
  evt.put(std::move(output));
}

void
RecoTauProducer::fillDescriptions(edm::ConfigurationDescriptions& descriptions) {
  // combinatoricRecoTaus
  edm::ParameterSetDescription desc;
  desc.add<edm::InputTag>("piZeroSrc", edm::InputTag("ak4PFJetsRecoTauPiZeros"));

  edm::ParameterSetDescription pset_signalQualityCuts;
  pset_signalQualityCuts.add<double>("maxDeltaZ", 0.4);
  pset_signalQualityCuts.add<double>("minTrackPt", 0.5);
  pset_signalQualityCuts.add<double>("minTrackVertexWeight", -1.0);
  pset_signalQualityCuts.add<double>("maxTrackChi2", 100.0);
  pset_signalQualityCuts.add<unsigned int>("minTrackPixelHits", 0);
  pset_signalQualityCuts.add<double>("minGammaEt", 1.0);
  pset_signalQualityCuts.add<unsigned int>("minTrackHits", 3);
  pset_signalQualityCuts.add<double>("minNeutralHadronEt", 30.0);
  pset_signalQualityCuts.add<double>("maxTransverseImpactParameter", 0.1);
  pset_signalQualityCuts.addOptional<bool>("useTracksInsteadOfPFHadrons");

  edm::ParameterSetDescription pset_vxAssocQualityCuts;
  pset_vxAssocQualityCuts.add<double>("minTrackPt", 0.5);
  pset_vxAssocQualityCuts.add<double>("minTrackVertexWeight", -1.0);
  pset_vxAssocQualityCuts.add<double>("maxTrackChi2", 100.0);
  pset_vxAssocQualityCuts.add<unsigned int>("minTrackPixelHits", 0);
  pset_vxAssocQualityCuts.add<double>("minGammaEt", 1.0);
  pset_vxAssocQualityCuts.add<unsigned int>("minTrackHits", 3);
  pset_vxAssocQualityCuts.add<double>("maxTransverseImpactParameter", 0.1);
  pset_vxAssocQualityCuts.addOptional<bool>("useTracksInsteadOfPFHadrons");

  edm::ParameterSetDescription pset_isolationQualityCuts;
  pset_isolationQualityCuts.add<double>("maxDeltaZ", 0.2);
  pset_isolationQualityCuts.add<double>("minTrackPt", 1.0);
  pset_isolationQualityCuts.add<double>("minTrackVertexWeight", -1.0);
  pset_isolationQualityCuts.add<double>("maxTrackChi2", 100.0);
  pset_isolationQualityCuts.add<unsigned int>("minTrackPixelHits", 0);
  pset_isolationQualityCuts.add<double>("minGammaEt", 1.5);
  pset_isolationQualityCuts.add<unsigned int>("minTrackHits", 8);
  pset_isolationQualityCuts.add<double>("maxTransverseImpactParameter", 0.03);
  pset_isolationQualityCuts.addOptional<bool>("useTracksInsteadOfPFHadrons");

  edm::ParameterSetDescription pset_qualityCuts;
  pset_qualityCuts.add<edm::ParameterSetDescription>("signalQualityCuts",    pset_signalQualityCuts);
  pset_qualityCuts.add<edm::ParameterSetDescription>("vxAssocQualityCuts",   pset_vxAssocQualityCuts);
  pset_qualityCuts.add<edm::ParameterSetDescription>("isolationQualityCuts", pset_isolationQualityCuts);
  pset_qualityCuts.add<std::string>("leadingTrkOrPFCandOption", "leadPFCand");
  pset_qualityCuts.add<std::string>("pvFindingAlgo", "closestInDeltaZ");
  pset_qualityCuts.add<edm::InputTag>("primaryVertexSrc", edm::InputTag("offlinePrimaryVertices"));
  pset_qualityCuts.add<bool>("vertexTrackFiltering", false);
  pset_qualityCuts.add<bool>("recoverLeadingTrk", false);

  {
    edm::ParameterSetDescription vpsd_modifiers;
    vpsd_modifiers.add<std::string>("name");
    vpsd_modifiers.add<std::string>("plugin");
    vpsd_modifiers.add<int>("verbosity", 0);

    vpsd_modifiers.add<edm::ParameterSetDescription>("qualityCuts", pset_qualityCuts);
    vpsd_modifiers.addOptional<edm::InputTag>("ElectronPreIDProducer");
    vpsd_modifiers.addOptional<std::string>("DataType");
    vpsd_modifiers.addOptional<double>("maximumForElectrionPreIDOutput");
    vpsd_modifiers.addOptional<double>("ElecPreIDLeadTkMatch_maxDR");
    vpsd_modifiers.addOptional<double>("EcalStripSumE_minClusEnergy");
    vpsd_modifiers.addOptional<double>("EcalStripSumE_deltaPhiOverQ_minValue");
    vpsd_modifiers.addOptional<double>("EcalStripSumE_deltaPhiOverQ_maxValue");
    vpsd_modifiers.addOptional<double>("EcalStripSumE_deltaEta");
    vpsd_modifiers.addOptional<double>("dRaddNeutralHadron");
    vpsd_modifiers.addOptional<double>("minGammaEt");
    vpsd_modifiers.addOptional<double>("dRaddPhoton");
    vpsd_modifiers.addOptional<double>("minNeutralHadronEt");
    vpsd_modifiers.addOptional<edm::InputTag>("pfTauTagInfoSrc");

    desc.addVPSet("modifiers", vpsd_modifiers);
  }

  desc.add<edm::InputTag>("jetRegionSrc", edm::InputTag("recoTauAK4PFJets08Region"));
  desc.add<double>("maxJetAbsEta", 2.5);
  desc.add<std::string>("outputSelection", "leadPFChargedHadrCand().isNonnull()");
  desc.add<edm::InputTag>("chargedHadronSrc", edm::InputTag("ak4PFJetsRecoTauChargedHadrons"));
  desc.add<double>("minJetPt", 14.0);
  desc.add<edm::InputTag>("jetSrc", edm::InputTag("ak4PFJets"));

  {
    edm::ParameterSetDescription vpsd_builders;
    vpsd_builders.add<std::string>("name");
    vpsd_builders.add<std::string>("plugin");
    vpsd_builders.add<int>("verbosity", 0);

    vpsd_builders.add<edm::ParameterSetDescription>("qualityCuts", pset_qualityCuts);
    {
      edm::ParameterSetDescription vpsd_decayModes;
      vpsd_decayModes.add<unsigned int>("nPiZeros", 0);
      vpsd_decayModes.add<unsigned int>("maxPiZeros", 0);
      vpsd_decayModes.add<unsigned int>("nCharged", 1);
      vpsd_decayModes.add<unsigned int>("maxTracks", 6);
      vpsd_builders.addVPSetOptional("decayModes", vpsd_decayModes);
    }
    vpsd_builders.add<double>("minAbsPhotonSumPt_insideSignalCone", 2.5);
    vpsd_builders.add<double>("minRelPhotonSumPt_insideSignalCone", 0.1);
    vpsd_builders.add<edm::InputTag>("pfCandSrc", edm::InputTag("particleFlow"));
    
    vpsd_builders.addOptional<std::string>("signalConeSize");
    vpsd_builders.addOptional<double>("isolationConeSize");
    vpsd_builders.addOptional<double>("minAbsPhotonSumPt_outsideSignalCone");
    vpsd_builders.addOptional<double>("minRelPhotonSumPt_outsideSignalCone");
    vpsd_builders.addOptional<std::string>("isoConeChargedHadrons");
    vpsd_builders.addOptional<std::string>("isoConeNeutralHadrons");
    vpsd_builders.addOptional<std::string>("isoConePiZeros");
    vpsd_builders.addOptional<double>("leadObjectPt");
    vpsd_builders.addOptional<std::string>("matchingCone");
    vpsd_builders.addOptional<int>("maxSignalConeChargedHadrons");
    vpsd_builders.addOptional<std::string>("signalConeChargedHadrons");
    vpsd_builders.addOptional<std::string>("signalConeNeutralHadrons");
    vpsd_builders.addOptional<std::string>("signalConePiZeros");
    vpsd_builders.addOptional<bool>("usePFLeptons");

    desc.addVPSet("builders", vpsd_builders);
  }

  desc.add<bool>("buildNullTaus", false);
  desc.add<int>("verbosity", 0);
  descriptions.add("combinatoricRecoTaus", desc);
}

#include "FWCore/Framework/interface/MakerMacros.h"
DEFINE_FWK_MODULE(RecoTauProducer);
