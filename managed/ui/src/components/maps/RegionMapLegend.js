// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { isEmptyArray } from 'utils/ObjectUtils';
import './stylesheets/RegionMapLegend.scss';
import SelectList from 'react-widgets/lib/SelectList';
import _ from 'lodash';

export default class RegionMapLegend extends Component{
  constructor(props) {
    super(props);
    this.state = {selectedProviderList: props.providers};
  }

  componentWillUpdate(nextProps, nextState) {
    if (!_.isEqual(this.state.selectedProviderList, nextState.selectedProviderList)) {
      this.props.onProviderSelect(nextState.selectedProviderList);
    }
  }
  render() {
    const self = this;
    if (isEmptyArray(self.props.providers)) {
      return <span />;
    }
    return (
      <div className="yb-region-map-legend">
        <h4>Cloud Providers</h4>
        <div>
          <span>Select
            <small>
              <span className="map-menu-selector" onClick={value => self.setState({selectedProviderList: self.props.providers})}>
                All
              </span>
              <span className="map-menu-selector" onClick={value => self.setState({selectedProviderList: []})}>None</span>
            </small>
          </span>
          <SelectList data={self.props.providers} valueField={"code"} textField={"name"}
                      value={self.state.selectedProviderList} multiple={true}
                      onChange={value => self.setState({selectedProviderList: value})} />
        </div>
      </div>
    );
  }
}
