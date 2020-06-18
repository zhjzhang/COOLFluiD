#include "FluxReconstructionTurb/NavierStokesGReKO2DSourceTerm_Lang.hh"
#include "Common/CFLog.hh"
#include "Framework/GeometricEntity.hh"
#include "Framework/MeshData.hh"
#include "FluxReconstructionTurb/FluxReconstructionKOmega.hh"
#include "Framework/SubSystemStatus.hh"
#include "FluxReconstructionTurb/KLogOmega2DSourceTerm.hh"
#include "GReKO/NavierStokes2DGReKLogOPuvt.hh"

#include "Framework/MethodCommandProvider.hh"
#include "MathTools/MathConsts.hh"
#include "KOmega/NavierStokesKLogOmegaVarSetTypes.hh"

//////////////////////////////////////////////////////////////////////////////

using namespace std;
using namespace COOLFluiD::Framework;
using namespace COOLFluiD::Common;
using namespace COOLFluiD::Physics::NavierStokes;
using namespace COOLFluiD::Physics::KOmega;
using namespace COOLFluiD::Physics::GReKO;

//////////////////////////////////////////////////////////////////////////////

namespace COOLFluiD {

    namespace FluxReconstructionMethod {

////////////////////////////////////////////////////////////////////////////////////////////////////

MethodCommandProvider<NavierStokesGReKO2DSourceTerm_Lang, FluxReconstructionSolverData, FluxReconstructionKOmegaModule>
NavierStokesGReKO2DSourceTerm_LangFRProvider("NavierStokesGReKO2DSourceTerm_Lang");

///////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::defineConfigOptions(Config::OptionList& options)
{
 options.addConfigOption< bool >("SSTV","True for SST with Vorticity source term");
 options.addConfigOption< bool >("SSTsust","True for SST with  sustaining terms");
 options.addConfigOption< CFreal >("Kinf","K at the farfield");
 options.addConfigOption< CFreal >("Omegainf","Omega at the farfield");
 options.addConfigOption< bool >("PGrad","pressure Gradient");
 options.addConfigOption< bool >("Decouple","Decouple y-ReTheta from k and log(omega), simply solving as fully turbulent.");
}

/////////////////////////////////////////////////////////////////////////////////////////////////////

NavierStokesGReKO2DSourceTerm_Lang::NavierStokesGReKO2DSourceTerm_Lang(const std::string& name) :
  KLogOmega2DSourceTerm(name),
  socket_gammaEff("gammaEff"),
  socket_gammaSep("gammaSep"),
  socket_fOnset("fOnset"),
  socket_fLength("fLength"),
  m_Rethetat(),
  m_Rethetac(),
  m_Flength(),
  m_vorticity(),
  m_strain()
{ 
  addConfigOptionsTo(this);
  
  m_SST_V = false;
  setParameter("SSTV",&m_SST_V);
  m_SST_sust = false;
  setParameter("SSTsust",&m_SST_sust);
  m_kamb = 100. ;
  setParameter("Kinf",&m_kamb);
  m_omegaamb = 0.1;
  setParameter("Omegainf",&m_omegaamb);
  
  m_PGrad = true;
  setParameter("PGrad",&m_PGrad);
  
  m_decouple = false;
  setParameter("Decouple",&m_decouple);
}

/////////////////////////////////////////////////////////////////////////////////////////////////

NavierStokesGReKO2DSourceTerm_Lang::~NavierStokesGReKO2DSourceTerm_Lang()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::setup()
{
  KLogOmega2DSourceTerm::setup();
  
  DataHandle< CFreal > gammaEff = socket_gammaEff.getDataHandle();
  
  DataHandle< CFreal > gammaSep = socket_gammaSep.getDataHandle();
  
  DataHandle< CFreal > fOnset = socket_fOnset.getDataHandle();
  
  DataHandle< CFreal > fLength = socket_fLength.getDataHandle();
  
  DataHandle< CFreal > wallDistance = socket_wallDistance.getDataHandle();
  
  // resize socket
  gammaEff.resize(wallDistance.size());
  gammaSep.resize(wallDistance.size());
  fOnset.resize(wallDistance.size());
  fLength.resize(wallDistance.size());
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::unsetup()
{
  KLogOmega2DSourceTerm::unsetup();
}

//////////////////////////////////////////////////////////////////////////////

std::vector< Common::SafePtr< BaseDataSocketSource > >
  NavierStokesGReKO2DSourceTerm_Lang::providesSockets()
{
  std::vector< Common::SafePtr< BaseDataSocketSource > > result = KLogOmega2DSourceTerm::providesSockets();
  result.push_back(&socket_gammaEff);
  result.push_back(&socket_gammaSep);
  result.push_back(&socket_fOnset);
  result.push_back(&socket_fLength);
  return result;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void  NavierStokesGReKO2DSourceTerm_Lang::getStrain(const CFreal VoverRadius, const CFuint iState)
{
  const CFreal gradU_X = (*(m_cellGrads[iState][1]))[XX];
  const CFreal gradU_Y = (*(m_cellGrads[iState][1]))[YY];
  const CFreal gradV_X = (*(m_cellGrads[iState][2]))[XX];
  const CFreal gradV_Y = (*(m_cellGrads[iState][2]))[YY];
  const CFreal gradSum = (gradU_Y + gradV_X);
  const CFreal strain = gradU_X*gradU_X + 0.5*gradSum*gradSum + gradV_Y*gradV_Y + VoverRadius ;
  m_strain = std::sqrt(2.*strain);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

void  NavierStokesGReKO2DSourceTerm_Lang::getVorticity(const CFuint iState)
{
  const CFreal Vorticity1 = (*(m_cellGrads[iState][2]))[XX] - (*(m_cellGrads[iState][1]))[YY];
  m_vorticity = fabs(Vorticity1);
}

//////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::addSourceTerm(RealVector& resUpdates)
{       
  const CFuint kID = (*m_cellStates)[0]->size() - 4;
  const CFuint omegaID = kID + 1;
  const CFuint gammaID = kID + 2;
  const CFuint ReTID = kID + 3;

  const CFuint iKPD = m_eulerVarSet->getModel()->getFirstScalarVar(0);
  
  const bool Puvt = getMethodData().getUpdateVarStr() == "Puvt";
    
  SafePtr< NavierStokes2DGReKLogOPuvt > navierStokesVarSet = m_diffVarSet.d_castTo< NavierStokes2DGReKLogOPuvt >();
   
  // get the gradients datahandle
  DataHandle< vector< RealVector > > gradients = socket_gradients.getDataHandle();

  // set gradients
  const CFuint nbrStates = m_cellStates->size();

  for (CFuint iState = 0; iState < nbrStates; ++iState)
  {
    const CFuint stateID = (*m_cellStates)[iState]->getLocalID();
    
    for (CFuint iEq = 0; iEq < m_nbrEqs; ++iEq)
    {
      *(m_cellGrads[iState][iEq]) = gradients[stateID][iEq];
    }
    
    // Get the wall distance
    DataHandle< CFreal > wallDist = socket_wallDistance.getDataHandle();

    m_currWallDist[iState] = wallDist[stateID];//((*m_cellStates)[iState]->getCoordinates())[YY];
  }
  
  const EquationSubSysDescriptor& eqData = PhysicalModelStack::getActive()->getEquationSubSysDescriptor();
  const CFuint nbEqs = eqData.getNbEqsSS();
  const CFuint totalNbEqs = PhysicalModelStack::getActive()->getNbEq(); 
  
  for (CFuint iSol = 0; iSol < nbrStates; ++iSol)
  {
    m_eulerVarSet->computePhysicalData(*((*m_cellStates)[iSol]), m_solPhysData);
      
    // Set the wall distance before computing the turbulent viscosity
    navierStokesVarSet->setWallDistance(m_currWallDist[iSol]);
    
    const CFreal mut = navierStokesVarSet->getTurbDynViscosityFromGradientVars(*((*m_cellStates)[iSol]), m_cellGrads[iSol]);
    const CFreal mu = navierStokesVarSet->getLaminarDynViscosityFromGradientVars(*((*m_cellStates)[iSol]));
    
    navierStokesVarSet->computeBlendingCoefFromGradientVars(*((*m_cellStates)[iSol]), *(m_cellGrads[iSol][kID]), *(m_cellGrads[iSol][omegaID]));
    
    // Get Vorticity
    getVorticity(iSol);

    const CFreal avV     = m_solPhysData[EulerTerm::V];
    const CFreal avK     = std::max(m_solPhysData[iKPD],0.0);
    const CFreal avOmega = std::exp(m_solPhysData[iKPD+1]);
    const CFreal avGa    = std::min(std::max(m_solPhysData[iKPD+2],0.0),1.0);
    const CFreal avRe    = std::max(m_solPhysData[iKPD+3],20.0);
    const CFreal rho = navierStokesVarSet->getDensity(*((*m_cellStates)[iSol]));
    
    ///Compute the blending function Fthetat
    const CFreal  Rew         = (rho * m_currWallDist[iSol] * m_currWallDist[iSol] * avOmega)/(mu);   
    const CFreal  Fwake1      = (1e-5 * Rew)*(1e-5 * Rew);   
    const CFreal  Fwake       = exp(-Fwake1);
    const CFreal  thetaBL     = (avRe*mu)/(rho*avV);
    const CFreal  deltaBL     = (0.5*15*thetaBL);
    const CFreal  delta       = (50.0 * m_vorticity * m_currWallDist[iSol] * deltaBL)/(avV);
    const CFreal  coefFtheta0 = (m_currWallDist[iSol]/delta)*(m_currWallDist[iSol]/delta)*(m_currWallDist[iSol]/delta)*(m_currWallDist[iSol]/delta);
    const CFreal  coefFtheta1 = exp(-coefFtheta0);
    const CFreal  Ftheta1     = Fwake * coefFtheta1;
    const CFreal  ce2         = 50.0;
    //const CFreal  overce2     = 1/50;
    //const CFreal  Ftheta2     = (avGa-overce2)/(1.0-overce2);
    const CFreal  Ftheta3     = 1-(((ce2*avGa-1.0)/(ce2-1.0))*((ce2*avGa-1.0)/(ce2-1.0)));
    const CFreal  Ftheta4     = std::max(Ftheta1,Ftheta3);
    const CFreal  Fthetat     = std::min(Ftheta4,1.0);

    //The variables needed for the  production term of Re
    const CFreal cthetat   = 0.03;
    const CFreal t         = (500.0 * mu )/(rho * avV * avV);
    cf_assert(avV > 0.);   
    const CFreal Tu = min(max(100.0 * (std::sqrt(2.0*avK/3.0))/(avV),0.027),100.0);
    
    if (!m_PGrad)
    {
      getRethetat(Tu);
    }
    else
    {
      getRethetatwithPressureGradient(mu,Tu,iSol); 
    }
    
    //Compute Flength
    getFlength(avRe);
    
    //Compute _Retheta_C
    getRethetac(avRe);
  
    //Compute Strain 
    getStrain(0.0,iSol);//_vOverRadius); 
    
    //compute Gasep
    const CFreal Rt         = (rho*avK)/(mu*avOmega);
    
    // error in FV!!
    const CFreal Freattach0 = std::pow(Rt/20.0,4);//exp(-Rt/20);
    const CFreal Freattach  = exp(-Freattach0);//std::pow(Freattach0,4);
    
    const CFreal Rev        = (rho*m_currWallDist[iSol]*m_currWallDist[iSol]*m_strain)/(mu);
    const CFreal Gasep1     = ((Rev)/((3.235 *  m_Rethetac)))-1.0;
    const CFreal Gasep2     = std::max(0.,Gasep1);
    const CFreal Gasep3     = 2.0*Gasep2*Freattach;
    const CFreal Gasep4     = std::min(Gasep3,2.0);
    const CFreal Gasep      = Gasep4*Fthetat;

    ///gammaEff
    CFreal gammaEff  = std::max(avGa,Gasep);

    // The Onset function of the  production term of the intermittency Ga
    
    // error in FV!!
    const CFreal  Fonset1 = (Rev )/(2.193*m_Rethetac);//(Rev )/(2.93*m_Rethetac);
    
    const CFreal  Fonset2 = std::pow(Fonset1,4);
    const CFreal  Fonset3 = std::max(Fonset1,Fonset2);
    const CFreal  Fonset4 = std::min(Fonset3,2.0);
    const CFreal  Fonset6 = 1-((Rt/2.5)*(Rt/2.5)*(Rt/2.5)) ;
    const CFreal  Fonset7 = std::max(Fonset6,0.);
    const CFreal  Fonset8 = (Fonset4 - Fonset7);
    const CFreal  Fonset  = std::max(Fonset8,0.);
    
    ///The Modified Destruction term: k
    ///CoeffDk This coefficient is used in the destruction term related to k: 
    const CFreal coeffDk1  = std::max(gammaEff,0.1);
    CFreal coeffDk   = std::min(coeffDk1,1.0);
    
    // check if we just want to solve as fully turbulent
    if (m_decouple) 
    {
      gammaEff = 1.0;
      coeffDk = 1.0;
    }
    
    // compute destruction terms for k and logOmega
    // Destruction term: k
    m_destructionTerm_k = (-1.) * rho * avOmega * avK * navierStokesVarSet->getBetaStar(*((*m_cellStates)[iSol]));
    m_destructionTerm_k *= coeffDk; 
  
    // Destruction term: Omega
    m_destructionTerm_Omega = (-1.) * rho * avOmega * navierStokesVarSet->getBeta(*((*m_cellStates)[iSol]));//(-1.) * rho * avOmega * avOmega * navierStokesVarSet->getBeta(*((*m_cellStates)[iState]));
  
//  if (m_currWallDist[iState] < 3.0e-3 && m_currWallDist[iState] > 2.8e-3 && !m_isPerturbed)
//  { 
//      CFLog(INFO, "WkB: " << Omega_desterm << ", beta: " << navierStokesVarSet->getBetaStar(*((*m_cellStates)[iState])) << ", rho: " << rho << ", fact: " << DcoFactor << "\n");
//  }
  
    // Make sure negative values dont propagate...
    m_destructionTerm_k     = std::min(0., m_destructionTerm_k );
    m_destructionTerm_Omega = std::min(0., m_destructionTerm_Omega);
        
    ///The Modified Production  terms for k and logOmega
    const CFuint uID = 1;//getStateVelocityIDs()[XX];
    const CFuint vID = 2;//getStateVelocityIDs()[YY];
    const CFreal dux = (*(m_cellGrads[iSol][uID]))[XX];
    const CFreal duy = (*(m_cellGrads[iSol][uID]))[YY]; 
    const CFreal dvx = (*(m_cellGrads[iSol][vID]))[XX]; 
    const CFreal dvy = (*(m_cellGrads[iSol][vID]))[YY]; 

    const CFreal coeffTauMu = navierStokesVarSet->getModel().getCoeffTau();
    const CFreal twoThirdRhoK = (2./3.)*(avK * rho);
  
    m_prodTerm_k = coeffTauMu*(mut*((4./3.)*((dux-dvy)*(dux-dvy)+(dux*dvy))
			         +(duy+dvx)*(duy+dvx)))
                                 -twoThirdRhoK*(dux+dvy);
  
    ///Production term: Omega
    const CFreal blendingCoefF1 = navierStokesVarSet->getBlendingCoefficientF1();
    const CFreal sigmaOmega2 = navierStokesVarSet->getSigmaOmega2();
  
    const CFreal overOmega = 1./avOmega;
    m_prodTerm_Omega  = (navierStokesVarSet->getGammaCoef()*rho/mut) * m_prodTerm_k * overOmega;
  
    if (m_limitP)
    {
      m_prodTerm_k     = std::min(10.*fabs(m_destructionTerm_k), m_prodTerm_k);
      m_prodTerm_Omega = std::min(10.*fabs(m_destructionTerm_k)*overOmega*(navierStokesVarSet->getGammaCoef()*rho/mut), m_prodTerm_Omega);
    }  
  
    ///This is used in (BSL,SST), not for normal kOmeg
    m_prodTerm_Omega += (1. - blendingCoefF1) * 2. * rho * overOmega * sigmaOmega2* ((*(m_cellGrads[iSol][kID]))[XX]*(*(m_cellGrads[iSol][omegaID]))[XX] + (*(m_cellGrads[iSol][kID]))[YY]*(*(m_cellGrads[iSol][omegaID]))[YY]);
  //OmegaProdTerm += (1. - blendingCoefF1) * 2. * rho * overOmega * sigmaOmega2* ((*(m_cellGrads[iState][kID]))[XX]*(*(m_cellGrads[iState][omegaID]))[XX] + (*(m_cellGrads[iState][kID]))[YY]*(*(m_cellGrads[iState][omegaID]))[YY]);
    //MathFunctions::innerProd(*(m_cellGrads[iState][kID]), *(m_cellGrads[iState][omegaID]));
//  OmegaProdTerm *= _Radius; 
     
    const CFreal coeffTauMu1 = coeffTauMu*mu;
    const CFreal coeffTauMu3 = coeffTauMu*mut*navierStokesVarSet->getSigmaOmega();//(blendingCoefF1 * navierStokesVarSet->getSigmaOmega1() + (1.0-blendingCoefF1)*sigmaOmega2);
  
    m_prodTerm_Omega += (coeffTauMu1 + coeffTauMu3)*((*(m_cellGrads[iSol][omegaID]))[XX]*(*(m_cellGrads[iSol][omegaID]))[XX] + (*(m_cellGrads[iSol][omegaID]))[YY]*(*(m_cellGrads[iSol][omegaID]))[YY]);
  
    ///gammaEff This coefficient is used in the destruction term related to k: 
    m_prodTerm_k *= gammaEff;
  
    //Make sure negative values dont propagate...
    m_prodTerm_k            = std::max(0., m_prodTerm_k);
    m_prodTerm_Omega        = std::max(0., m_prodTerm_Omega);
         
//    //Limit the production terms
//    m_prodTerm_k     = std::min(10.*fabs(m_destructionTerm_k), m_prodTerm_k);
//    m_prodTerm_Omega = std::min(10.*fabs(m_destructionTerm_Omega), m_prodTerm_Omega);

    // The production term of the intermittency Ga
    const CFreal ca1       = 2.0;
    const CFreal ce1       = 1.0;
    const CFreal ca2 =  0.06;
    const CFreal GaFonset1 = avGa * Fonset;
    const CFreal GaFonset  = sqrt(GaFonset1);
    
    // This is missing in FV wrt Flength!!!
    const CFreal FSubLayer0 = Rew/200.0 * Rew/200.0;
    const CFreal FSubLayer = exp(-FSubLayer0);
    const CFreal FlengthTot = m_Flength * (1.0 - FSubLayer) + 40.0*FSubLayer;

    CFreal prodTerm_Ga = FlengthTot * ca1 * rho * m_strain * GaFonset * (1.0 - ce1*avGa);
  
    // The production term of  Re
    CFreal prodTerm_Re = cthetat * (rho/t) * (m_Rethetat - avRe) * (1.0 - Fthetat);

    //The variables needed for the  Destruction term of Ga   
    // error in FV!!!
    const CFreal  Fturb1 =  pow(Rt/4.0,4);//exp(-Rt/4); 
    const CFreal  Fturb =  exp(-Fturb1);//std::pow(Fturb1,4); 
    
    //Destruction term of the intermittency Ga
    CFreal  destructionTerm_Ga  = (-1.0) *ca2 * rho *  m_vorticity * avGa * Fturb * (ce2*avGa - 1.0);
  
    //destructionTerm_Ga *= m_Radius;
    
    //Destruction term of Re
    CFreal destructionTerm_Re = 0.0;

    ///Make sure negative values dont propagate
    prodTerm_Ga        = max(0., prodTerm_Ga);
    prodTerm_Re        = max(0., prodTerm_Re);
    destructionTerm_Ga = min(0., destructionTerm_Ga);
    //destructionTerm_Re = min(0., destructionTerm_Re);
      
    /// Compute the rhs contribution
    // and Store the unperturbed source terms
    resUpdates[m_nbrEqs*iSol + kID] = m_prodTerm_k + m_destructionTerm_k;
    resUpdates[m_nbrEqs*iSol + omegaID] = m_prodTerm_Omega + m_destructionTerm_Omega;
    resUpdates[m_nbrEqs*iSol + gammaID] = prodTerm_Ga + destructionTerm_Ga;
    resUpdates[m_nbrEqs*iSol + ReTID] = prodTerm_Re + destructionTerm_Re;
    
    resUpdates[m_nbrEqs*iSol + 3] = -m_prodTerm_k - m_destructionTerm_k;
    
    if (!m_isPerturbed)
    {
      // store the shear stress velocity
      DataHandle< CFreal > wallShearStressVelocity = socket_wallShearStressVelocity.getDataHandle();
      
      DataHandle< CFreal > gammaEffSocket = socket_gammaEff.getDataHandle();
      
      DataHandle< CFreal > gammaSep = socket_gammaSep.getDataHandle();
      
      DataHandle< CFreal > fOnset = socket_fOnset.getDataHandle();
      
      DataHandle< CFreal > fLength = socket_fLength.getDataHandle();
      
      const CFreal mu = navierStokesVarSet->getLaminarDynViscosityFromGradientVars(*((*m_cellStates)[iSol]));
      
      const CFreal rho = navierStokesVarSet->getDensity(*((*m_cellStates)[iSol]));
    
      const CFreal nuTot = (mu + mut)/rho;
      
      const CFreal dUdY = (*(m_cellGrads[iSol][1]))[YY];
            
      // take the absolute value of dUdY to avoid nan which causes tecplot to be unable to load the file
      wallShearStressVelocity[(((*m_cellStates)[iSol]))->getLocalID()] = sqrt(nuTot*fabs(dUdY));//m_prodTerm_Omega;//std::min(m_prodTerm_Omega,200.0);//m_prodTerm_Omega;//
      
      gammaEffSocket[(((*m_cellStates)[iSol]))->getLocalID()] = gammaEff;
      
      gammaSep[(((*m_cellStates)[iSol]))->getLocalID()] = Gasep;
      
      fOnset[(((*m_cellStates)[iSol]))->getLocalID()] = Fonset;//Rev;//
      
      fLength[(((*m_cellStates)[iSol]))->getLocalID()] = FlengthTot;//m_Rethetac;//
      
//      if (m_currWallDist[iSol] < 3.0e-3 && m_currWallDist[iSol] > 2.8e-3)
//  { 
//      CFLog(INFO, "Pk: " << m_prodTerm_k << ", Dk: " << m_destructionTerm_k << ", Pw: " << m_prodTerm_Omega << ", Dw: " << m_destructionTerm_Omega << ", d: " << m_currWallDist[iSol] <<"\n");
//  }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::getRethetac(const CFreal Retheta)
{	
/// errors here in FV!
//  if (Retheta <= 1860)
//  {
//    m_Rethetac  = Retheta - (396.035*1e-2 -120.656*1e-4*Retheta)+(868.230*1e-6)*Retheta*Retheta 
//                          - 696.506*1e-9*Retheta*Retheta*Retheta + 174.105*1e-12*Retheta*Retheta*Retheta*Retheta;
//  }
//  else 
//  {
//    m_Rethetac = Retheta - 593.11 + (Retheta - 1870.0)*0.482;
//  }
  
  if (Retheta <= 1870)
  {
    m_Rethetac  = -396.035e-2 + 10120.656e-4*Retheta - 868.23e-6*Retheta*Retheta 
                          + 696.506e-9*Retheta*Retheta*Retheta - 174.105e-12*Retheta*Retheta*Retheta*Retheta;
  }
  else 
  {
    m_Rethetac = Retheta - (593.11 + (Retheta - 1870.0)*0.482);
  }
}           

/////////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::getFlength(const CFreal Retheta)
{
  if (Retheta < 400)
  {  
    m_Flength = 39.8189 - 119.27e-4*Retheta - 132.567e-6*Retheta*Retheta;   
  }
  else if ((Retheta >= 400 ) && (Retheta < 596)) 
  {
    m_Flength = 263.404 - 123.939e-2*Retheta + 194.548e-5*Retheta*Retheta - 101.695e-8*Retheta*Retheta*Retheta;
  }
  else if ((Retheta >= 596 ) && (Retheta < 1200)) 
  {
    m_Flength = 0.5 - (Retheta - 596.0)*3.0e-4;
  }
  else 
  {
    m_Flength = 0.3188;
  }
}
 
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::getRethetat(const CFreal Tu) 
{
  cf_assert(Tu >= 0.0);   
  
  const CFreal overTu = 1.0/Tu;
           
  if (Tu<=1.3) 
  {
    m_Rethetat = (1173.51-589.428*Tu + 0.2196*overTu*overTu);
  }
  else 
  {
    const CFreal lamco5   = Tu - 0.5658;
    const CFreal pwtu   = -0.671;
    
    m_Rethetat = 331.5*std::pow(lamco5,pwtu);
  }
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::getLambda(CFreal& lambda, const CFreal theta, const CFreal viscosity, const CFuint iState)
{
  SafePtr< NavierStokes2DGReKLogOPuvt > navierStokesVarSet = m_diffVarSet.d_castTo< NavierStokes2DGReKLogOPuvt >();

  const CFreal avV = m_solPhysData[EulerTerm::V];   //AvrageSpeeed;   
  const CFreal mu = viscosity;
  const CFreal rho = navierStokesVarSet->getDensity(*((*m_cellStates)[iState])); 
  const CFreal rhoovermu = rho/mu;   
  const CFreal avu = m_solPhysData[EulerTerm::VX];
  const CFreal avv = m_solPhysData[EulerTerm::VY];        
  const CFreal overU2 = 1./(avV*avV);//1./avV;
  
  // error in FV!!
  const CFreal dUdsTerm = avu*avu*(*(m_cellGrads[iState][1]))[XX] + avv*avv*(*(m_cellGrads[iState][2]))[YY] + avu*avv*((*(m_cellGrads[iState][1]))[YY] + (*(m_cellGrads[iState][2]))[XX]);//avV * (avu* (*(m_cellGrads[iState][1]))[XX]  + avv * (*(m_cellGrads[iState][2]))[XX]);
  //const CFreal dUdyTerm = avV * (avu* (*(m_cellGrads[iState][1]))[YY]  + avv * (*(m_cellGrads[iState][2]))[YY]);
  const CFreal dUds = overU2 * dUdsTerm;
        
  const CFreal theta_sq = theta * theta;
  const CFreal lambda0 =  rhoovermu * theta_sq * dUds;

  CFreal lambda1 = std::max(lambda0,-0.1);
  lambda = std::min(lambda1,0.1);
}        

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::getFlambda(const CFreal lambda, const CFreal Tu, CFreal& Flambda, const CFreal theta, bool Prime )
{
  // error in FV!!
  const CFreal lambdaprime = lambda*2.0/theta;
  
  // error in FV!!!
  if (lambda <= 0.0) 
  {
    const CFreal lamco1  = (Tu/1.5);
    const CFreal lamco2  = -1.0*std::pow(lamco1,1.5);
    const CFreal Flamb = 12.986 * lambda + 123.66 * lambda*lambda + 405.689 * lambda*lambda*lambda;
    const CFreal Flambprime = 12.986 + 2 * 123.66 * lambda + 3 * 405.689 * lambda*lambda;
    
    Flambda = (!Prime)? 1 + (Flamb * std::exp(lamco2)): Flambprime * std::exp(lamco2) * lambdaprime;
  }
  else 
  {
    const CFreal lamco3 = -1.0*(Tu/0.5);
    const CFreal lamco4   = -35.0*lambda;
    const CFreal FlambP   = 0.275*(1-std::exp(lamco4))*std::exp(lamco3);
    
    // error in FV!!
    const CFreal FlambprimeP = 0.275*35.0*std::exp(lamco4)*std::exp(lamco3); 
    
    Flambda = (!Prime)? 1 + FlambP : FlambprimeP*lambdaprime;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void NavierStokesGReKO2DSourceTerm_Lang::getRethetatwithPressureGradient(const CFreal viscosity, const CFreal Tu, const CFuint iState)
{
  SafePtr< NavierStokes2DGReKLogOPuvt > navierStokesVarSet = m_diffVarSet.d_castTo< NavierStokes2DGReKLogOPuvt >();

  const CFreal avV = m_solPhysData[EulerTerm::V];   //AvrageSpeeed;     
  const CFreal mu = viscosity;                                          
  const CFreal rho = navierStokesVarSet->getDensity(*((*m_cellStates)[iState]));                 
  const CFreal overTu = 1./Tu; 
  
  // get start value by assuming Flambda = 1
  getRethetat(Tu);
  
  CFreal thetaLimitFactor = 0.0;
  getLambda(thetaLimitFactor, 1., viscosity, iState);
  
  const CFreal thetaLimit = sqrt(0.1/fabs(thetaLimitFactor));
  
  CFreal theta0 = min(max((mu/(rho*avV))*m_Rethetat,0.0),thetaLimit);//(mu/(rho*avV))*m_Rethetat;
  //if (theta0 == thetaLimit) theta0 = thetaLimit/5.0;
  CFreal theta1 = 0.0;
 
//        vector<CFreal> Theta(2);
  CFreal lambda = 0;
  CFreal Flambda = 0;
  CFreal FlambdaPrime = 0;
        
//         if(Tu <=1.3){
//    	     Theta[0] = (mu/(rho*avV))*(1173.51-589.428*Tu + 0.2196*overTu*overTu);
//  	   }
//  	else {
//   	     const CFreal lamco5   = Tu - 0.5658;
//   	     const CFreal pwtu     = -0.671;
//    	     Theta[0]            = (mu/(rho*avV))*331.5*std::pow(lamco5,pwtu);
//   	   }

  const CFuint MAXITER   = 10;
  const CFreal TOL       = 1e-6;
  
  for (CFuint iter = 0; iter < MAXITER; ++iter)
  {
    CFreal restheta = std::fabs(theta0*TOL);

    //The variables needed for the calculation of Re_thetat
    // compute new lambda
    getLambda(lambda, theta0, viscosity, iState);
    
    // compute new Flambda
    getFlambda(lambda,Tu,Flambda,theta0,false);  
    
    getFlambda(lambda,Tu,FlambdaPrime,theta0,true);  
          
//         if (Tu<=1.3) {
//         	 CFreal Rethetat0 = (1173.51-589.428*Tu + 0.2196*overTu*overTu);
//                 m_Rethetat = Rethetat0 * Flambda;
//         }
//   	else {
//     	      const CFreal lamco5   = Tu - 0.5658;
//   	      const CFreal pwtu   = -0.671;
//              const CFreal Rethetat0 = 331.5*std::pow(lamco5,pwtu);
//              m_Rethetat = Rethetat0 * Flambda;
// 	 }

    const CFreal Rethetatprime = m_Rethetat*FlambdaPrime;
     
    const CFreal mainF = m_Rethetat*Flambda - (rho*avV)*theta0/mu;
    const CFreal mainFprime = Rethetatprime - (rho*avV)/mu;
    
    theta1 = min(max(theta0 - mainF/mainFprime,0.0),thetaLimit);
    
//    if (fabs(theta1) > thetaLimit)
//    {
//      theta1 = thetaLimit;
//    }
    
   	//cout.precision(20); cout << "diff   " << Theta[1]/Theta[0] << endl;
    if ( fabs(theta0-theta1) < restheta || iter == MAXITER-1) 
    {
      // compute new lambda
      getLambda(lambda, theta1, viscosity, iState);
    
      // compute new Flambda
      getFlambda(lambda,Tu,Flambda,theta1,false);  
    m_FLambda = Flambda;
      m_Rethetat *= Flambda;
      
      const CFreal RethetaCheck = ((rho*avV)/mu)*theta1;
      
      if (iter == MAXITER-1) CFLog(INFO, "Max iter thetat reached!\n");
      if (fabs((RethetaCheck-m_Rethetat)/m_Rethetat) > 100.0*TOL) 
      {
          CFLog(INFO, "Error of Re_thetat: " << (RethetaCheck-m_Rethetat)/m_Rethetat << "\n");
          CFLog(INFO, "theta1: " << theta1 << ", theta0: " << theta0 << ", thetaLim: " << thetaLimit << ", realTh1: " << theta0 - mainF/mainFprime << ", thetaStart: " << (mu/(rho*avV))*m_Rethetat/Flambda << "\n");
      }
          
      break;
    }
    
    theta0 = theta1;
  }
}

//////////////////////////////////////////////////////////////////////////////

    } // namespace FluxReconstructionMethod

} // namespace COOLFluiD

//////////////////////////////////////////////////////////////////////////////
