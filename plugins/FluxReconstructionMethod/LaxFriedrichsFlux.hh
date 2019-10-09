#ifndef COOLFluiD_FluxReconstructionMethod_LaxFriedrichsFlux_hh
#define COOLFluiD_FluxReconstructionMethod_LaxFriedrichsFlux_hh

//////////////////////////////////////////////////////////////////////////////

#include "Framework/BaseMethodStrategyProvider.hh"
#include "FluxReconstructionMethod/FluxReconstructionSolverData.hh"
#include "FluxReconstructionMethod/RiemannFlux.hh"
#include "Framework/VarSetTransformer.hh"
#include <stdio.h>

#ifdef CF_HAVE_CUDA
#include "Common/CUDA/CudaEnv.hh"
#include "Framework/MathTypes.hh"
#include "Framework/VarSetTransformerT.hh"
#include "FluxReconstructionMethod/FluxData.hh"
#endif

//////////////////////////////////////////////////////////////////////////////

namespace COOLFluiD {

  namespace FluxReconstructionMethod {

//////////////////////////////////////////////////////////////////////////////

/**
 * This class represent a Lax-Friedrichs/Rusanov flux
 *
 * @author Ray Vandenhoeck
 */
class LaxFriedrichsFlux : public RiemannFlux {

public:  // methods
    
  #ifdef CF_HAVE_CUDA
  /// nested class defining local options
  template <typename P = NOTYPE>
  class DeviceConfigOptions {
  public:
    /// constructor
    HOST_DEVICE DeviceConfigOptions() {}
    
    /// destructor
    HOST_DEVICE ~DeviceConfigOptions() {}
    
    /// initialize with another object of the same kind
    HOST_DEVICE void init(DeviceConfigOptions<P> *const in) 
    {
    }
  };
  
  /// nested class defining a functor
  template <DeviceType DT, typename VS>
  class DeviceFunc {
  public:
    typedef VS MODEL;
    typedef LaxFriedrichsFlux BASE;
    
    /// constructor taking the options as argument
    HOST_DEVICE DeviceFunc(DeviceConfigOptions<NOTYPE>* dco) : m_dco(dco) {}
 
    /// Compute needed private variables that do not depend on the physical state
    HOST_DEVICE void prepareComputation(FluxData<VS>* data, VS* model) {}
   
    /// Compute the flux : implementation
    HOST_DEVICE void operator()(FluxData<VS>* data, VS* model, bool isInterface, const CFuint iSol, const CFuint cellID); 
    
  private:
    DeviceConfigOptions<NOTYPE>* m_dco;
    typename MathTypes<CFreal, DT, VS::NBEQS>::VEC m_tmp;
    typename MathTypes<CFreal, DT, VS::NBEQS>::VEC m_tmp2;
    typename MathTypes<CFreal, DT, VS::NBEQS>::VEC m_tmpState;
    typename MathTypes<CFreal, DT, VS::NBEQS>::VEC m_tmpState2;
    typename MathTypes<CFreal, DT, VS::DATASIZE>::VEC m_pdata;
    typename MathTypes<CFreal, DT, VS::DATASIZE>::VEC m_pdata2;
    typename MathTypes<CFreal, DT, VS::DIM*VS::DIM>::VEC m_tempUnitNormal;
    typename MathTypes<CFreal, DT, VS::DIM>::VEC m_tempFlxUnitNormal;
  };
  
  /// copy the local configuration options to the device
  void copyConfigOptionsToDevice(DeviceConfigOptions<NOTYPE>* dco) 
  {
  }  
  
  /// copy the local configuration options to the device
  void copyConfigOptions(DeviceConfigOptions<NOTYPE>* dco) 
  {
  }   
#endif

  /// Constructor
  LaxFriedrichsFlux(const std::string& name);

  /// Destructor
  ~LaxFriedrichsFlux();
 
  /// Compute the Riemann flux, from left and right states and a normal vector
  virtual RealVector& computeFlux(Framework::State& lState,
				  Framework::State& rState,
				  const RealVector& normal);

  /// Compute the Riemann flux, from left and right states and a normal vector
  virtual RealVector& computeFlux(Framework::State& lState, RealVector& lExtraVars,
                                  Framework::State& rState, RealVector& rExtraVars,
                                  const RealVector& normal);


  /// Gets the Class name
  static std::string getClassName()
  {
    return "LaxFriedrichsFlux";
  }

  /// Set up private data and data
  virtual void setup();
  
  /// Unset up private data and data
  virtual void unsetup();

private: // data

  /// array storing the sum of the right and left flux
  RealVector  m_sumFlux;


}; // class LaxFriedrichsFlux

//////////////////////////////////////////////////////////////////////////////

#ifdef CF_HAVE_CUDA
/// nested class defining the flux
template <DeviceType DT, typename VS>
void LaxFriedrichsFlux::DeviceFunc<DT, VS>::operator()(FluxData<VS>* data, VS* model, bool isInterface, const CFuint iSol, const CFuint cellID) 
{
  if (isInterface)
  {
    typename VS::UPDATE_VS* updateVS = model->getUpdateVS();
//printf("ok2\n");
    // right physical data, flux
    //for (CFuint iEq = 0; iEq < VS::NBEQS; ++iEq){printf("state %d, var %d: %f\n",iSol, iEq, &m_pdata[0]);}
    updateVS->computePhysicalData(data->getLstate(iSol), &m_pdata[0]);
    updateVS->computePhysicalData(data->getRstate(iSol), &m_pdata2[0]);

    typename MathTypes<CFreal,DT,VS::DIM>::SLICEVEC unitNormal(data->getFlxScaledNormal(iSol));
    m_tempFlxUnitNormal = unitNormal;
    
    typename MathTypes<CFreal,DT,VS::NBEQS>::SLICEVEC flux(data->getInterfaceFlux(iSol));
        
    updateVS->getFlux(&m_pdata[0], &m_tempFlxUnitNormal[0], &m_tmp[0]); 
    updateVS->getFlux(&m_pdata2[0], &m_tempFlxUnitNormal[0], &m_tmp2[0]); 


    
    const CFreal lMaxAbsEVal = updateVS->getMaxAbsEigenValue(&m_pdata[0], &m_tempFlxUnitNormal[0]);
    const CFreal rMaxAbsEVal = updateVS->getMaxAbsEigenValue(&m_pdata2[0], &m_tempFlxUnitNormal[0]);

    const CFreal absA = 0.5*(lMaxAbsEVal+rMaxAbsEVal);
    
    Framework::VarSetTransformerT<typename VS::UPDATE_VS, typename VS::SOLUTION_VS, NOTYPE>* up2Sol = model->getUpdateToSolution();
  
    // transform to solution variables
    up2Sol->transform(data->getLstate(iSol), &m_tmpState[0]);
    up2Sol->transform(data->getRstate(iSol), &m_tmpState2[0]);
  
    typename MathTypes<CFreal,DT,VS::NBEQS>::SLICEVEC stateL(&m_tmpState[0]);
    typename MathTypes<CFreal,DT,VS::NBEQS>::SLICEVEC stateR(&m_tmpState2[0]); 
      
    for (CFuint iEq = 0; iEq < VS::NBEQS; ++iEq)
    {
      flux[iEq] = 0.5*(m_tmp[iEq]+m_tmp2[iEq]) - 0.5*absA*(stateR[iEq] - stateL[iEq]);
//if (cellID == 11) printf("flux %f, var %d, lState %f, rState %f, lflux %f, rflux %f, absA %f, normalX %f, normalY %f\n",flux[iEq],iEq,stateL[iEq],stateR[iEq],m_tmp[iEq],m_tmp2[iEq],absA,m_tempFlxUnitNormal[0],m_tempFlxUnitNormal[1]);
    }
//    printf("ok3\n");
  }
  else
  {
    typename VS::UPDATE_VS* updateVS = model->getUpdateVS();

    // right physical data, flux
    updateVS->computePhysicalData(data->getState(iSol), &m_pdata[0]);
//printf("LFBefore\n");
    typename MathTypes<CFreal,DT,VS::DIM*VS::DIM>::SLICEVEC unitNormal(data->getScaledNormal(iSol));
    m_tempUnitNormal = unitNormal;
    for (CFuint iDim = 0; iDim < VS::DIM; ++iDim)
    {
      typename MathTypes<CFreal,DT,VS::NBEQS>::SLICEVEC flux(data->getFlux(iSol,iDim));
        
      updateVS->getFlux(&m_pdata[0], &m_tempUnitNormal[iDim*VS::DIM], &m_tmp[0]); 
      
      for (CFuint iEq = 0; iEq < VS::NBEQS; ++iEq)
      {
        flux[iEq] = m_tmp[iEq];
      }
    }
  }
}
#endif

//////////////////////////////////////////////////////////////////////////////

  }  // namespace FluxReconstructionMethod

}  // namespace COOLFluiD

//////////////////////////////////////////////////////////////////////////////

#endif  // COOLFluiD_FluxReconstructionMethod_LaxFriedrichsFlux_hh
