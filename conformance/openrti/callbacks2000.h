  // ---- Inserted by conformance/openrti (callbacks2000.h) ------------------
  // IEEE 1516-2010 callbacks forwarded to their IEEE 1516-2000 forms, which
  // the ported tests override: the supplemental information is dropped, the
  // sent regions select the region variants of reflectAttributeValues.

  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandleValueMap const &,
                                      rti1516e::VariableLengthData const &, rti1516e::OrderType,
                                      rti1516e::TransportationType)
  { }
  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandleValueMap const &,
                                      rti1516e::VariableLengthData const &, rti1516e::OrderType,
                                      rti1516e::TransportationType, rti1516e::RegionHandleSet const &)
  { }
  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandleValueMap const &,
                                      rti1516e::VariableLengthData const &, rti1516e::OrderType,
                                      rti1516e::TransportationType, rti1516e::LogicalTime const &, rti1516e::OrderType)
  { }
  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandleValueMap const &,
                                      rti1516e::VariableLengthData const &, rti1516e::OrderType,
                                      rti1516e::TransportationType, rti1516e::LogicalTime const &, rti1516e::OrderType,
                                      rti1516e::RegionHandleSet const &)
  { }
  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandleValueMap const &,
                                      rti1516e::VariableLengthData const &, rti1516e::OrderType,
                                      rti1516e::TransportationType, rti1516e::LogicalTime const &, rti1516e::OrderType,
                                      rti1516e::MessageRetractionHandle)
  { }
  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandleValueMap const &,
                                      rti1516e::VariableLengthData const &, rti1516e::OrderType,
                                      rti1516e::TransportationType, rti1516e::LogicalTime const &, rti1516e::OrderType,
                                      rti1516e::MessageRetractionHandle, rti1516e::RegionHandleSet const &)
  { }

  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle o, rti1516e::AttributeHandleValueMap const & v,
                                      rti1516e::VariableLengthData const & tag, rti1516e::OrderType sent,
                                      rti1516e::TransportationType t, rti1516e::SupplementalReflectInfo info)
    RTI_THROW ((rti1516e::FederateInternalError))
  {
    if (info.hasSentRegions)
      reflectAttributeValues(o, v, tag, sent, t, info.sentRegions);
    else
      reflectAttributeValues(o, v, tag, sent, t);
  }
  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle o, rti1516e::AttributeHandleValueMap const & v,
                                      rti1516e::VariableLengthData const & tag, rti1516e::OrderType sent,
                                      rti1516e::TransportationType t, rti1516e::LogicalTime const & time,
                                      rti1516e::OrderType received, rti1516e::SupplementalReflectInfo info)
    RTI_THROW ((rti1516e::FederateInternalError))
  {
    if (info.hasSentRegions)
      reflectAttributeValues(o, v, tag, sent, t, time, received, info.sentRegions);
    else
      reflectAttributeValues(o, v, tag, sent, t, time, received);
  }
  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle o, rti1516e::AttributeHandleValueMap const & v,
                                      rti1516e::VariableLengthData const & tag, rti1516e::OrderType sent,
                                      rti1516e::TransportationType t, rti1516e::LogicalTime const & time,
                                      rti1516e::OrderType received, rti1516e::MessageRetractionHandle h,
                                      rti1516e::SupplementalReflectInfo info)
    RTI_THROW ((rti1516e::FederateInternalError))
  {
    if (info.hasSentRegions)
      reflectAttributeValues(o, v, tag, sent, t, time, received, h, info.sentRegions);
    else
      reflectAttributeValues(o, v, tag, sent, t, time, received, h);
  }

  virtual void receiveInteraction(rti1516e::InteractionClassHandle, rti1516e::ParameterHandleValueMap const &,
                                  rti1516e::VariableLengthData const &, rti1516e::OrderType, rti1516e::TransportationType)
  { }
  virtual void receiveInteraction(rti1516e::InteractionClassHandle, rti1516e::ParameterHandleValueMap const &,
                                  rti1516e::VariableLengthData const &, rti1516e::OrderType, rti1516e::TransportationType,
                                  rti1516e::LogicalTime const &, rti1516e::OrderType)
  { }
  virtual void receiveInteraction(rti1516e::InteractionClassHandle, rti1516e::ParameterHandleValueMap const &,
                                  rti1516e::VariableLengthData const &, rti1516e::OrderType, rti1516e::TransportationType,
                                  rti1516e::LogicalTime const &, rti1516e::OrderType, rti1516e::MessageRetractionHandle)
  { }
  virtual void receiveInteraction(rti1516e::InteractionClassHandle c, rti1516e::ParameterHandleValueMap const & v,
                                  rti1516e::VariableLengthData const & tag, rti1516e::OrderType sent,
                                  rti1516e::TransportationType t, rti1516e::SupplementalReceiveInfo)
    RTI_THROW ((rti1516e::FederateInternalError))
  { receiveInteraction(c, v, tag, sent, t); }
  virtual void receiveInteraction(rti1516e::InteractionClassHandle c, rti1516e::ParameterHandleValueMap const & v,
                                  rti1516e::VariableLengthData const & tag, rti1516e::OrderType sent,
                                  rti1516e::TransportationType t, rti1516e::LogicalTime const & time,
                                  rti1516e::OrderType received, rti1516e::SupplementalReceiveInfo)
    RTI_THROW ((rti1516e::FederateInternalError))
  { receiveInteraction(c, v, tag, sent, t, time, received); }
  virtual void receiveInteraction(rti1516e::InteractionClassHandle c, rti1516e::ParameterHandleValueMap const & v,
                                  rti1516e::VariableLengthData const & tag, rti1516e::OrderType sent,
                                  rti1516e::TransportationType t, rti1516e::LogicalTime const & time,
                                  rti1516e::OrderType received, rti1516e::MessageRetractionHandle h,
                                  rti1516e::SupplementalReceiveInfo)
    RTI_THROW ((rti1516e::FederateInternalError))
  { receiveInteraction(c, v, tag, sent, t, time, received, h); }

  virtual void removeObjectInstance(rti1516e::ObjectInstanceHandle, rti1516e::VariableLengthData const &,
                                    rti1516e::OrderType)
  { }
  virtual void removeObjectInstance(rti1516e::ObjectInstanceHandle, rti1516e::VariableLengthData const &,
                                    rti1516e::OrderType, rti1516e::LogicalTime const &, rti1516e::OrderType)
  { }
  virtual void removeObjectInstance(rti1516e::ObjectInstanceHandle, rti1516e::VariableLengthData const &,
                                    rti1516e::OrderType, rti1516e::LogicalTime const &, rti1516e::OrderType,
                                    rti1516e::MessageRetractionHandle)
  { }
  virtual void removeObjectInstance(rti1516e::ObjectInstanceHandle o, rti1516e::VariableLengthData const & tag,
                                    rti1516e::OrderType sent, rti1516e::SupplementalRemoveInfo)
    RTI_THROW ((rti1516e::FederateInternalError))
  { removeObjectInstance(o, tag, sent); }
  virtual void removeObjectInstance(rti1516e::ObjectInstanceHandle o, rti1516e::VariableLengthData const & tag,
                                    rti1516e::OrderType sent, rti1516e::LogicalTime const & time,
                                    rti1516e::OrderType received, rti1516e::SupplementalRemoveInfo)
    RTI_THROW ((rti1516e::FederateInternalError))
  { removeObjectInstance(o, tag, sent, time, received); }
  virtual void removeObjectInstance(rti1516e::ObjectInstanceHandle o, rti1516e::VariableLengthData const & tag,
                                    rti1516e::OrderType sent, rti1516e::LogicalTime const & time,
                                    rti1516e::OrderType received, rti1516e::MessageRetractionHandle h,
                                    rti1516e::SupplementalRemoveInfo)
    RTI_THROW ((rti1516e::FederateInternalError))
  { removeObjectInstance(o, tag, sent, time, received, h); }
  // ---- end of callbacks2000.h ----------------------------------------------

