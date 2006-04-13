// Implements class MultichannelCorrelator.  
// CCDOC COMMENTS NEED TO ULTIMATELY BE PASTED INTO INCLUDE FILE
//
//

/*  This is a simple object intentionally limited to the scope of this file.
It is used to find the peak correlation value.  Simple idea that could be
made generic eventually, but not worth the trouble at this point. 
It is used by the MultichannelCorrelator object which is why it is placed 
in this file and at the top.
*/

#include "MultichannelCorrelator.h"
#include "seispp.h"

//class TimeSeriesMaximum
//{
//public:
//	double lag;
//	double peak;
//	TimeSeriesMaximum(){lag=0.0;peak=0.0;};
//	TimeSeriesMaximum(TimeSeries& d);
//	TimeSeriesMaximum& operator=(const TimeSeriesMaximum& other);
//};
// This algorithm might be replaceable with stl find algorithm.  Check
TimeSeriesMaximum::TimeSeriesMaximum(TimeSeries& d)
{
	int j,jlag;
	peak=d.s[0];
	jlag=0;
	for(j=1;j<d.s.size();++j)
	{
		if(peak<d.s[j])
		{
			jlag=j;
			peak=d.s[j];
		}
	}
	lag=d.time(jlag);
}
// this is essential what the compiler would generate automatically, but
// better safe than sorry.
TimeSeriesMaximum& TimeSeriesMaximum::operator=(const TimeSeriesMaximum& other)
{
	if(this!=&other)
	{
		peak=other.peak;
		lag=other.lag;
	}
	return(*this);
}
// The beam needs to have this normalization constant set.  All statics are computed
// relative to this amplitude using the dot product of the beam with data
// This files scope global sets the name used for this
//
const string mdscalename("beam_scale_factor");
double ComputeAmplitudeStatic(TimeSeries& beam, TimeSeries& data, int lag)
{
	double beam_scale=beam.get_double(mdscalename);
	// Because used internally won't best bounds, but don't use this in
	// another program without making it safe in that sense.
	double datamp=ddot(beam.s.size(),&(beam.s[0]),1,&(data.s[lag]),1);
	return(datamp/beam_scale);
}
double linfnorm(vector<double> x)
{
        int i;
	double xmax;

	for(i=1,xmax=fabs(x[0]);i<x.size();++i)
		xmax=max(fabs(x[i]),xmax);
	return(xmax);
}
	
//@{
// Default constructor.  Probably could leave defaulted but better safe than sorry.
//
//@}
MultichannelCorrelator:: MultichannelCorrelator()
{
	lag.resize(0);
	peakxcor.resize(0);
	weight.resize(0);
	amplitude_static.resize(0);
	pfunction_used=NoPfunction;
	xcor.member.resize(0);
}
/*********************************************************
* Helper function for RobustSNR method below.  With real data
* cross correlation will often yield an absurd result due to 
* noisy data or multiple cycle skips.  We use a simple hard 
* cutoff in the absolute value of the computed lag.  Whenever
* the computed lag is larger than this cutoff data are marked
* bad in a somewhat obscure way.  That is, we set the moveout
* to a constant called MoveoutBad, which is essentially a very
* large value.  The stacker drops any data for which the moveout
* given is outside its input data range (what else could it do)
* so this somewhat obscure method works.  It must be recognized
* by anyone attempting to utilize the software in some other
* context, however, as it is not the most straightforward way
* to signal this problem.  
*
* args:
*  data - trace data which lag was computed for
*  lag - vector of time lags computed from data object
*  lag_cutoff - fabs of lags larger than this are marked bad.
*
*  Note this routine also sets the moveout metadata member for
* ALL members of data.  
***********************************************************/
void kill_data_with_bad_xcor(TimeSeriesEnsemble& data,
		vector<double>lag,double lag_cutoff)
{
	double median_lag=median(lag);
//
// We assume any lag larger than this cutoff is 
// an error.  We probably should drop such data, but
// for now we simply reset the lag to 0 and assume
// the robust method will handle this.
//
	for(int i=0;i<data.member.size();++i)
	{
		if(data.member[i].live)
		{
		// mark data with unrealistic lags as bad
		// by this mechanism
			if(fabs(lag[i]-median_lag)>lag_cutoff)
			{
				lag[i]=MoveoutBad;
				data.member[i].put(moveout_keyword,MoveoutBad);
			}
			else
			{
				data.member[i].put(moveout_keyword,lag[i]);
			}
		}
	}
}
//@{
// Constructor that implements MultichannelCorrelation with different choices
// of algorithm.
// This constructor takes the contents of in input ensemble and performs
// a multichannel correlation with the results depending on the choice of 
// algorithm and, in anything but the "Basic" method, the choice of the
// initial estimate of the array beam.  
//
// The "Basic" method does a simple cross correlation of each trace with a
// reference trace passed either through the initial_beam parameter or the 
// reference_member parameter.  Other methods involve an iterative procedure
// using a beam recomputed iteratively until convergence.  The basic algorithm
// is correlate with beam, align stack using current lags, sum, repeat until
// time shifts do not change.  
//
// The robust methods differ from the simpler methods in the use of a penalty 
// function in forming the stack.  Currently two robust stacks are supported:
// (a) a median stack and (b) a SNR weighting method.  The later is of greatest
// value for highly variable signal to noise conditions.  The most significant 
// example is source correlations where different magnitude events can enter in
// the same stack.  Note the SNR scheme is slightly more complicated than a signal
// to noise estimate.  It is more coherence-like using a ratio of L2 norm of the
// original data to the L2 norm of the residuals (amplitude adjusted stack) to 
// define the SNR.  
//
//@}

MultichannelCorrelator:: MultichannelCorrelator(TimeSeriesEnsemble& data,
	CorrelationMethod method, 
		TimeWindow beam_window,
			TimeWindow robust_window,
			    double lag_cutoff,
				StackType stacktype,
					TimeSeries *initial_beam,
						int reference_member,
							bool normalize,
								bool parallel)
{
	int i, count;
	double tshiftnorm;
	double TSCONVERGE;
	const int MAXIT=30;
	const string base_message("MultichannelCorrelator constructor:  ");
	const double LAG_RANGE_MULTIPLIER(2.0);  // Correlation range is limited to this times lag_cutoff
	TimeWindow lag_range(-LAG_RANGE_MULTIPLIER*lag_cutoff,LAG_RANGE_MULTIPLIER*lag_cutoff);

	if(data.member.empty())
		throw SeisppError(base_message+string("Input data ensemble is empty\n"));
	// All methods use reference_member when that argument is used.  Otherwise
	// the contents of the Beam trace are used.
	try{
		if((initial_beam==NULL) && (reference_member<0) )
			throw SeisppError(base_message+string("Illegal input.\n")
				+ string("Require either a TimeSeries initial trace or index to ensemble member.\n"));
	
		if(initial_beam!=NULL)
		{
			beam=WindowData(*initial_beam,beam_window);
		}
		else
		{
			//used to be data.s
			beam=WindowData(data.member[reference_member],beam_window);
			// The beam needs to be normalized for efficiency
			double rms=dnrm2(beam.s.size(),&(beam.s[0]),1);
			dscal(beam.s.size(),rms,&(beam.s[0]),1);
			beam.put(mdscalename,rms);
		}
		TSCONVERGE=1.5*beam.dt;
		//
		// All methods do an initial correlation against the beam.
		// Even the basic method does this because the beam is a selected
		// station or some input reference trace passed as initial_beam.  
		//
		// correlation function should have a normalized boolean to allows
		// normalization to be turned on or off.
		//
		for(i=0;i<data.member.size();++i)
		{
			xcor.member.push_back(correlation(beam,data.member[i],lag_range,true));
			if(xcor.member[i].live)
			{
				TimeSeriesMaximum tsm(xcor.member[i]);
				lag.push_back(tsm.lag);
				peakxcor.push_back(tsm.peak);
				weight.push_back(1.0);
				amplitude_static.push_back(ComputeAmplitudeStatic(beam,data.member[i],tsm.lag));
				// This is needed for the stacker to work correctly below
				data.member[i].put(moveout_keyword,tsm.lag);
			}
			else
			{
				lag.push_back(0.0);
				peakxcor.push_back(0.0);
				weight.push_back(0.0);
				amplitude_static.push_back(0.0);
				// signal the stacker this is a bad
				// by using a very large moveout value
				data.member[i].put(moveout_keyword,MoveoutBad);
			}
		}
		kill_data_with_bad_xcor(data,lag,lag_cutoff);
		// holds lag estimated in previous iteration below
		vector<double>lastlag=lag;
		// This vector holds changes in lag from last iteration.
		// deltalag is set to incorrect values here but this
		// is a convenient way to size the vector correctly.
		// It is cleared before use below.
		//
		vector<double>deltalag=lastlag; 
			
		method_used=method;
		//switch(CorrelationMethod)
		switch(method)
		{
		case Basic:
		case SimpleStack:
			pfunction_used=NoPfunction;
			break;
		case RobustStack:
			pfunction_used=StackCoherence;
			break;
		default:
			cerr << "Multichannel correlator:  unknown method"
				<< "Defaulting to RobustStack method"<<endl;
			pfunction_used=StackCoherence;
			method_used=RobustStack;
		}
		//
		// Note one odd bit of logic here.  Event he simple
		// method passes through this loop once.  Note the
		// conditional that loopback only occurs when method
		// is set to RobustStack.
		//
		count=0;
		Stack newstack;
		double stack_normalization_factor;
		do {
			if( (method==Basic) || (method==SimpleStack) )
				newstack=Stack(data,beam_window);
			else
				newstack=Stack(data,beam_window,robust_window,stacktype);	
			beam=newstack.stack;
			// The current Stack object has data normalized
			// in a way that this constant is not needed so
			// it is set to 1.0
			beam.put(mdscalename,1.0);  
			beam.put("fold",newstack.fold);
			stack_normalization_factor=linfnorm(newstack.weights);
			// silently avoid divide by zero.  Shouldn't happen but worth this 
			// safety valve.
			if(stack_normalization_factor<=0.0) stack_normalization_factor=1.0;
			//
			// Note it is safe to use operator [] on the vectors
			// in the loop below because we used push_back
			// to initialize them all above.  
			//
			for(i=0;i<data.member.size();++i)
			{
				xcor.member[i]=correlation(beam,data.member[i],lag_range,true);
				// Stack object handles data marked bad
				// already.  Have to do same here as 
				// xcor has all zeros if data had a gap
				if(xcor.member[i].live)
				{
					TimeSeriesMaximum tsm(xcor.member[i]);
					lag[i]=tsm.lag;
					peakxcor[i]=tsm.peak;
					weight[i]=newstack.weights[i]/stack_normalization_factor;
					amplitude_static[i] = ComputeAmplitudeStatic(beam,
										data.member[i],tsm.lag);
					//useful to post these to metadata as well as in this data
					// structure
					data.member[i].put(amplitude_static_keyword,amplitude_static[i]);
					data.member[i].put(stack_weight_keyword,weight[i]);
					data.member[i].put(moveout_keyword,tsm.lag);
					data.member[i].put(peakxcor_keyword,tsm.peak);
				}
				else
				{
					lag[i]=0.0;
					peakxcor[i]=0.0;
					weight[i]=0.0;
					amplitude_static[i]=0.0;
					// this may not be necessary, but is safer
					data.member[i].put(amplitude_static_keyword,0.0);
					data.member[i].put(stack_weight_keyword,0.0);
					data.member[i].put(moveout_keyword,
						lag[i]);
					data.member[i].put(peakxcor_keyword,
						peakxcor[i]);
				}
				deltalag[i]=lag[i]-lastlag[i];
			} 
			kill_data_with_bad_xcor(data,lag,lag_cutoff);
			
			tshiftnorm=linfnorm(deltalag);
			lastlag=lag;
			count++;
		} while((tshiftnorm > TSCONVERGE)
				&& (count < MAXIT ));
		//
		// We need to add these attributes to the beam
		// so it has bare minimum need to be saved to 
		// a database
		//
		beam.put("wfdisc.time",beam.t0);
		beam.put("wfdisc.endtime",beam.endtime());
		beam.put("wfdisc.samprate",1.0/beam.dt);
		beam.put("wfdisc.nsamp",beam.ns);
		// These are defaults and would normally be changed
		beam.put("wfdisc.dir",string("."));
		beam.put("wfdisc.dfile",string("xcorbeam.w"));
		// Duplicated for alternative save strategy.
		// Current uses all uses this table instead of wfdisc
		beam.put("wfprocess.time",beam.t0);
		beam.put("wfprocess.endtime",beam.endtime());
		beam.put("wfprocess.timetype",string("r"));
		beam.put("wfprocess.samprate",1.0/beam.dt);
		beam.put("wfprocess.nsamp",beam.ns);
		beam.put("wfprocess.algorithm","dbxcor");
		beam.put("wfprocess.dir",string("."));
		beam.put("wfprocess.dfile",string("xcorbeam.w"));

	} catch (...) {
	    throw;
	};
}
//MultichannelCorrelator:: MultichannelCorrelator(ThreeComponentEnsemble data,
//	CorrelationMethod method, int component, bool parallel=false);
MultichannelCorrelator:: MultichannelCorrelator(ThreeComponentEnsemble data,
	CorrelationMethod method, 
		TimeWindow beam_window,
			int component,
				TimeWindow robust_window,
			            double lag_cutoff,
					StackType stacktype,
						TimeSeries *initial_beam,
							int reference_member,
								bool normalize,
									bool parallel)
{
//Peng Wang, commented out for compilation and testing, this should not be
//directly data or ThreeComponentEnsemble, instead, should converted to ThreeComponentSeismogram
//	try{
//		auto_ptr<TimeSeriesEnsemble>comp(ExtractComponent(data,component));
//		*this = MultichannelCorrelator(*comp,method,beam_window,robust_window,
//				stacktype,initial_beam,reference_member,normalize,parallel);
//	}catch(...) {
	    //supposed to throw something
//	}
}
MultichannelCorrelator:: MultichannelCorrelator(const MultichannelCorrelator& co)
{
	lag=co.lag;
	peakxcor=co.peakxcor;
	weight=co.weight;
	amplitude_static=co.amplitude_static;
	method_used=co.method_used;
	pfunction_used=co.pfunction_used;
	xcor=co.xcor;
	beam=co.beam;
}
MultichannelCorrelator& MultichannelCorrelator::operator=(const MultichannelCorrelator& co)
{
	if(this!=&co)
	{
		lag=co.lag;
		peakxcor=co.peakxcor;
		weight=co.weight;
		amplitude_static=co.amplitude_static;
		method_used=co.method_used;
		pfunction_used=co.pfunction_used;
		xcor=co.xcor;
		beam=co.beam;
	}
	return(*this);
}